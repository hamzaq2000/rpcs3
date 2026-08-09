#pragma once

#include "util/types.hpp"
#include "Utilities/geometry.h"
#include "TextureUtils.h"
#include "../rsx_utils.h"
#include "Emu/Memory/vm.h"

#define ENABLE_SURFACE_CACHE_DEBUG 0

namespace rsx
{
	enum surface_state_flags : u32
	{
		ready = 0,
		erase_bkgnd = 1,
		require_resolve = 2,
		require_unresolve = 4
	};

	enum class surface_sample_layout : u32
	{
		null = 0,
		ps3 = 1
	};

	enum class surface_inheritance_result : u32
	{
		none = 0,
		partial,
		full
	};

	// Half-open host-pixel rectangle written to a render target. This is used only
	// by the framebuffer-feedback profiling oracle; it does not affect rendering.
	struct surface_content_write_rect
	{
		u32 x1 = 0;
		u32 y1 = 0;
		u32 x2 = 0;
		u32 y2 = 0;

		bool valid() const
		{
			return x1 < x2 && y1 < y2;
		}
	};

	enum class surface_content_history_status : u8
	{
		complete,
		unavailable,
		unknown_or_full,
		overflow,
		broken_chain,
		generation_mismatch,
	};

	template <typename surface_type>
	struct surface_overlap_info_t
	{
		surface_type surface = nullptr;
		u32 base_address = 0;
		bool is_depth = false;
		bool is_clipped = false;

		coordu src_area;
		coordu dst_area;
	};

	template <typename surface_type>
	struct deferred_clipped_region
	{
		u16 src_x, src_y, dst_x, dst_y, width, height;
		f32 transfer_scale_x, transfer_scale_y;
		surface_type target;
		surface_type source;

		template <typename T>
		deferred_clipped_region<T> cast() const
		{
			deferred_clipped_region<T> ret;
			ret.src_x = src_x;
			ret.src_y = src_y;
			ret.dst_x = dst_x;
			ret.dst_y = dst_y;
			ret.width = width;
			ret.height = height;
			ret.transfer_scale_x = transfer_scale_x;
			ret.transfer_scale_y = transfer_scale_y;
			ret.target = static_cast<T>(target);
			ret.source = static_cast<T>(source);

			return ret;
		}

		operator bool() const
		{
			return (source != nullptr);
		}

		template <typename T>
		void init_transfer(T target_surface)
		{
			if (!width)
			{
				// Perform intersection here
				const auto region = rsx::get_transferable_region(target_surface);

				auto src_w = std::get<0>(region);
				auto src_h = std::get<1>(region);
				auto dst_w = std::get<2>(region);
				auto dst_h = std::get<3>(region);

				// Apply resolution scale if needed
				auto src = static_cast<T>(source);
				std::tie(src_w, src_h) = rsx::apply_resolution_scale<true>(
					src->resolution_scaling_config,
					src_w, src_h,
					src->template get_surface_width<rsx::surface_metrics::pixels>(),
					src->template get_surface_height<rsx::surface_metrics::pixels>());

				std::tie(dst_w, dst_h) = rsx::apply_resolution_scale<true>(
					target_surface->resolution_scaling_config,
					dst_w, dst_h,
					target_surface->template get_surface_width<rsx::surface_metrics::pixels>(),
					target_surface->template get_surface_height<rsx::surface_metrics::pixels>());

				width = src_w;
				height = src_h;
				transfer_scale_x = f32(dst_w) / src_w;
				transfer_scale_y = f32(dst_h) / src_h;

				target = target_surface;
			}
		}

		areai src_rect() const
		{
			ensure(width);
			return { src_x, src_y, src_x + width, src_y + height };
		}

		areai dst_rect() const
		{
			ensure(width);
			return { dst_x, dst_y, dst_x + u16(width * transfer_scale_x + 0.5f), dst_y + u16(height * transfer_scale_y + 0.5f) };
		}
	};

	struct surface_content_tracker
	{
	private:
		// Cell ready-backing receipts validate this from a non-RSX thread.
		// Keep generation publication lock-free and monotonic across that reader.
		atomic_t<u64> m_content_generation{0};

		struct content_write_event
		{
			u64 before = 0;
			u64 after = 0;
			surface_content_write_rect rect{};
			bool exact = false;
		};

		struct content_write_journal
		{
			static constexpr u32 capacity = 64;

			std::array<content_write_event, capacity> events{};
			u32 first = 0;
			u32 count = 0;
			bool overflowed = false;
		};

		// Most surfaces never participate in the debug oracle, so keep the fixed
		// ring itself lazily allocated. Once enabled, every generation transition is
		// recorded conservatively until an eligible draw refines it below.
		mutable std::unique_ptr<content_write_journal> m_content_write_journal;

	public:
		u64 mark_content_written()
		{
			const u64 before = m_content_generation;

			// Keep zero reserved for resources whose content generation is unknown.
			if (++m_content_generation == 0)
			{
				++m_content_generation;
			}

			if (auto* journal = m_content_write_journal.get())
			{
				u32 index;
				if (journal->count < content_write_journal::capacity)
				{
					index = (journal->first + journal->count) % content_write_journal::capacity;
					journal->count++;
				}
				else
				{
					index = journal->first;
					journal->first = (journal->first + 1) % content_write_journal::capacity;
					journal->overflowed = true;
				}

				journal->events[index] =
				{
					.before = before,
					.after = m_content_generation,
				};
			}

			return m_content_generation;
		}

		u64 get_content_generation() const
		{
			return m_content_generation;
		}

		void enable_content_write_journal() const
		{
			if (!m_content_write_journal)
			{
				m_content_write_journal = std::make_unique<content_write_journal>();
			}
		}

		bool refine_last_content_write(u64 before, u64 after, const surface_content_write_rect& rect)
		{
			auto* journal = m_content_write_journal.get();
			if (!journal || !journal->count || !rect.valid())
			{
				return false;
			}

			const u32 index = (journal->first + journal->count - 1) % content_write_journal::capacity;
			auto& event = journal->events[index];
			if (event.before != before || event.after != after)
			{
				return false;
			}

			event.rect = rect;
			event.exact = true;
			return true;
		}

		template <typename Visitor>
		surface_content_history_status visit_content_writes(
			u64 from, u64 through, Visitor&& visitor, u32* visited = nullptr) const
		{
			if (visited)
			{
				*visited = 0;
			}

			if (!from || !through || through != m_content_generation)
			{
				return surface_content_history_status::generation_mismatch;
			}

			if (from == through)
			{
				return surface_content_history_status::complete;
			}

			const auto* journal = m_content_write_journal.get();
			if (!journal || !journal->count)
			{
				return surface_content_history_status::unavailable;
			}

			u32 offset = 0;
			for (; offset < journal->count; ++offset)
			{
				const u32 index = (journal->first + offset) % content_write_journal::capacity;
				if (journal->events[index].before == from)
				{
					break;
				}
			}

			if (offset == journal->count)
			{
				return journal->overflowed
					? surface_content_history_status::overflow
					: surface_content_history_status::unavailable;
			}

			u64 expected = from;
			for (; offset < journal->count; ++offset)
			{
				const u32 index = (journal->first + offset) % content_write_journal::capacity;
				const auto& event = journal->events[index];

				if (event.before != expected)
				{
					return surface_content_history_status::broken_chain;
				}

				if (visited)
				{
					++*visited;
				}

				if (!event.exact)
				{
					return surface_content_history_status::unknown_or_full;
				}

				visitor(event.rect);
				expected = event.after;
				if (expected == through)
				{
					return surface_content_history_status::complete;
				}
			}

			return surface_content_history_status::broken_chain;
		}
	};

	template <typename image_storage_type>
	struct render_target_descriptor : public rsx::ref_counted, public surface_content_tracker
	{
		u64 last_use_tag = 0;         // tag indicating when this block was last confirmed to have been written to
		u32 base_addr = 0;

#if (ENABLE_SURFACE_CACHE_DEBUG)
		u64 memory_hash = 0;
#else
		std::array<std::pair<u32, u64>, 3> memory_tag_samples;
#endif

		std::vector<deferred_clipped_region<image_storage_type>> old_contents;

		// Surface properties
		u32 rsx_pitch = 0;
		u32 native_pitch = 0;
		u16 surface_width = 0;
		u16 surface_height = 0;
		u8  spp = 1;
		u8  samples_x = 1;
		u8  samples_y = 1;

		// Scaling configuration
		surface_scaling_config_t resolution_scaling_config;

		rsx::address_range32 memory_range;

		std::unique_ptr<typename std::remove_pointer_t<image_storage_type>> resolve_surface;
		surface_sample_layout sample_layout = surface_sample_layout::null;
		surface_raster_type raster_type = surface_raster_type::linear;

		flags32_t memory_usage_flags = surface_usage_flags::unknown;
		flags32_t state_flags = surface_state_flags::ready;
		flags32_t msaa_flags = surface_state_flags::ready;
		flags32_t stencil_init_flags = 0;

		union
		{
			rsx::surface_color_format gcm_color_format;
			rsx::surface_depth_format2 gcm_depth_format;
		}
		format_info;

		struct
		{
			u64 timestamp = 0;
			bool locked = false;
		}
		texture_cache_metadata;

		render_target_descriptor() {}

		virtual ~render_target_descriptor()
		{
			if (!old_contents.empty())
			{
				// Cascade resource derefs
				rsx_log.error("Resource was destroyed whilst holding a resource reference!");
			}
		}

		virtual image_storage_type get_surface(rsx::surface_access access_type) = 0;
		virtual bool is_depth_surface() const = 0;

		void reset()
		{
			texture_cache_metadata = {};
		}

		template<rsx::surface_metrics Metrics = rsx::surface_metrics::pixels, typename T = u32>
		T get_surface_width() const
		{
			if constexpr (Metrics == rsx::surface_metrics::samples)
			{
				return static_cast<T>(surface_width * samples_x);
			}
			else if constexpr (Metrics == rsx::surface_metrics::pixels)
			{
				return static_cast<T>(surface_width);
			}
			else if constexpr (Metrics == rsx::surface_metrics::bytes)
			{
				return static_cast<T>(native_pitch);
			}
			else
			{
				fmt::throw_exception("Unreachable");
			}
		}

		template<rsx::surface_metrics Metrics = rsx::surface_metrics::pixels, typename T = u32>
		T get_surface_height() const
		{
			if constexpr (Metrics == rsx::surface_metrics::samples)
			{
				return static_cast<T>(surface_height * samples_y);
			}
			else if constexpr (Metrics == rsx::surface_metrics::pixels)
			{
				return static_cast<T>(surface_height);
			}
			else if constexpr (Metrics == rsx::surface_metrics::bytes)
			{
				return static_cast<T>(surface_height * samples_y);
			}
			else
			{
				fmt::throw_exception("Unreachable");
			}
		}

		inline u32 get_rsx_pitch() const
		{
			return rsx_pitch;
		}

		inline u32 get_native_pitch() const
		{
			return native_pitch;
		}

		inline u8 get_bpp() const
		{
			return u8(get_native_pitch() / get_surface_width<rsx::surface_metrics::samples>());
		}

		inline u8 get_spp() const
		{
			return spp;
		}

		void set_aa_mode(rsx::surface_antialiasing aa)
		{
			switch (aa)
			{
				case rsx::surface_antialiasing::center_1_sample:
					samples_x = samples_y = spp = 1;
					break;
				case rsx::surface_antialiasing::diagonal_centered_2_samples:
					samples_x = spp = 2;
					samples_y = 1;
					break;
				case rsx::surface_antialiasing::square_centered_4_samples:
				case rsx::surface_antialiasing::square_rotated_4_samples:
					samples_x = samples_y = 2;
					spp = 4;
					break;
				default:
					fmt::throw_exception("Unknown AA mode 0x%x", static_cast<u8>(aa));
			}
		}

		void set_spp(u8 count)
		{
			switch (count)
			{
			case 1:
				samples_x = samples_y = spp = 1;
				break;
			case 2:
				samples_x = spp = 2;
				samples_y = 1;
				break;
			case 4:
				samples_x = samples_y = 2;
				spp = 4;
				break;
			default:
				fmt::throw_exception("Unexpected sample count 0x%x", count);
			}
		}

		void set_format(rsx::surface_color_format format)
		{
			format_info.gcm_color_format = format;
		}

		void set_format(rsx::surface_depth_format2 format)
		{
			format_info.gcm_depth_format = format;
		}

		void set_resolution_scaling_config(const surface_scaling_config_t& config)
		{
			resolution_scaling_config = config;
		}

		inline rsx::surface_color_format get_surface_color_format() const
		{
			return format_info.gcm_color_format;
		}

		inline rsx::surface_depth_format2 get_surface_depth_format() const
		{
			return format_info.gcm_depth_format;
		}

		inline u32 get_gcm_format() const
		{
			return
			(
				is_depth_surface() ?
					get_compatible_gcm_format(format_info.gcm_depth_format).first :
					get_compatible_gcm_format(format_info.gcm_color_format).first
			);
		}

		inline const rsx::surface_scaling_config_t& get_resolution_scaling_config() const
		{
			return resolution_scaling_config;
		}

		inline bool dirty() const
		{
			return (state_flags != rsx::surface_state_flags::ready) || !old_contents.empty();
		}

		inline bool write_through() const
		{
			return (state_flags & rsx::surface_state_flags::erase_bkgnd) && old_contents.empty();
		}

#if (ENABLE_SURFACE_CACHE_DEBUG)
		u64 hash_block() const
		{
			const auto padding = (rsx_pitch - native_pitch) / 8;
			const auto row_length = (native_pitch) / 8;
			auto num_rows = (surface_height * samples_y);
			auto ptr = reinterpret_cast<u64*>(vm::g_sudo_addr + base_addr);

			auto col = row_length;
			u64 result = 0;

			while (num_rows--)
			{
				while (col--)
				{
					result ^= *ptr++;
				}

				ptr += padding;
				col = row_length;
			}

			return result;
		}

		void queue_tag(u32 address)
		{
			ensure(native_pitch);
			ensure(rsx_pitch);
			mark_content_written();

			base_addr = address;

			const u32 internal_height = get_surface_height<rsx::surface_metrics::samples>();
			const u32 excess = (rsx_pitch - native_pitch);
			memory_range = rsx::address_range32::start_length(base_addr, internal_height * rsx_pitch - excess);
		}

		void sync_tag()
		{
			memory_hash = hash_block();
		}

		void shuffle_tag()
		{
			memory_hash = ~memory_hash;
		}

		bool test() const
		{
			return hash_block() == memory_hash;
		}

#else
		void queue_tag(u32 address)
		{
			ensure(native_pitch);
			ensure(rsx_pitch);
			mark_content_written();

			// Clear metadata
			reset();

			base_addr = address;

			const u32 size_x = (native_pitch > 8)? (native_pitch - 8) : 0u;
			const u32 size_y = u32(surface_height * samples_y) - 1u;
			const position2u samples[] =
			{
				// NOTE: Sorted by probability to catch dirty flag
				{0, 0},
				{size_x, size_y},
				{size_x / 2, size_y / 2},

				// Auxilliary, highly unlikely to ever catch anything
				// NOTE: Currently unused as length of samples is truncated to 3
				{size_x, 0},
				{0, size_y},
			};

			for (uint n = 0; n < memory_tag_samples.size(); ++n)
			{
				const auto sample_offset = (samples[n].y * rsx_pitch) + samples[n].x;
				memory_tag_samples[n].first = (sample_offset + base_addr);
			}

			const u32 internal_height = get_surface_height<rsx::surface_metrics::samples>();
			const u32 excess = (rsx_pitch - native_pitch);
			memory_range = rsx::address_range32::start_length(base_addr, internal_height * rsx_pitch - excess);
		}

		void sync_tag()
		{
			for (auto &e : memory_tag_samples)
			{
				e.second = *reinterpret_cast<nse_t<u64, 1>*>(vm::g_sudo_addr + e.first);
			}
		}

		void shuffle_tag()
		{
			memory_tag_samples[0].second = ~memory_tag_samples[0].second;
		}

		bool test() const
		{
			for (const auto& e : memory_tag_samples)
			{
				if (e.second != *reinterpret_cast<nse_t<u64, 1>*>(vm::g_sudo_addr + e.first))
					return false;
			}

			return true;
		}
#endif

		void invalidate_GPU_memory()
		{
			// Here be dragons. Use with caution.
			mark_content_written();
			shuffle_tag();
			state_flags |= rsx::surface_state_flags::erase_bkgnd;
		}

		void clear_rw_barrier()
		{
			for (auto &e : old_contents)
			{
				ensure(dynamic_cast<rsx::ref_counted*>(e.source))->release();
			}

			old_contents.clear();
		}

		template <typename T>
		u32 prepare_rw_barrier_for_transfer(T *target)
		{
			if (old_contents.size() <= 1)
				return 0;

			// Sort here before doing transfers since surfaces may have been updated in the meantime
			std::sort(old_contents.begin(), old_contents.end(), [](const auto& a, const auto &b)
			{
				const auto _a = static_cast<const T*>(a.source);
				const auto _b = static_cast<const T*>(b.source);
				return (_a->last_use_tag < _b->last_use_tag);
			});

			// Try and optimize by omitting possible overlapped transfers
			for (usz i = old_contents.size() - 1; i > 0 /* Intentional */; i--)
			{
				old_contents[i].init_transfer(target);

				const auto dst_area = old_contents[i].dst_rect();
				if (unsigned(dst_area.x2) == target->width() && unsigned(dst_area.y2) == target->height() &&
					!dst_area.x1 && !dst_area.y1)
				{
					// This transfer will overwrite everything older
					return u32(i);
				}
			}

			return 0;
		}

		template<typename T>
		void set_old_contents(T* other)
		{
			ensure(old_contents.empty());

			if (!other || other->get_rsx_pitch() != this->get_rsx_pitch())
			{
				return;
			}

			mark_content_written();

			old_contents.emplace_back();
			old_contents.back().source = other;
			other->add_ref();
		}

		template<typename T>
		void set_old_contents_region(const T& region, bool normalized)
		{
			// NOTE: This method will not perform pitch verification!
			ensure(region.source);
			ensure(region.source != static_cast<decltype(region.source)>(this));
			mark_content_written();

			old_contents.push_back(region.template cast<image_storage_type>());
			auto &slice = old_contents.back();
			region.source->add_ref();

			// Reverse normalization process if needed
			if (normalized)
			{
				const u16 bytes_to_texels_x = region.source->get_bpp() * region.source->samples_x;
				const u16 rows_to_texels_y = region.source->samples_y;
				slice.src_x /= bytes_to_texels_x;
				slice.src_y /= rows_to_texels_y;
				slice.width /= bytes_to_texels_x;
				slice.height /= rows_to_texels_y;

				const u16 bytes_to_texels_x2 = (get_bpp() * samples_x);
				const u16 rows_to_texels_y2 = samples_y;
				slice.dst_x /= bytes_to_texels_x2;
				slice.dst_y /= rows_to_texels_y2;

				slice.transfer_scale_x = f32(bytes_to_texels_x) / bytes_to_texels_x2;
				slice.transfer_scale_y = f32(rows_to_texels_y) / rows_to_texels_y2;
			}

			// Apply resolution scale if needed
			if (resolution_scaling_config.scale_percent != 100 ||
				region.source->resolution_scaling_config.scale_percent != 100)
			{
				const auto& src_res_scale = region.source->resolution_scaling_config;
				const auto& dst_res_scale = resolution_scaling_config;
				const auto src_surface = ensure(dynamic_cast<const render_target_descriptor*>(slice.source));
				const auto dst_surface = ensure(dynamic_cast<const render_target_descriptor*>(slice.target));

				auto [src_width, src_height] = rsx::apply_resolution_scale<true>(src_res_scale, slice.width, slice.height, src_surface->get_surface_width(), src_surface->get_surface_height());
				auto [dst_width, dst_height] = rsx::apply_resolution_scale<true>(dst_res_scale, slice.width, slice.height, dst_surface->get_surface_width(), dst_surface->get_surface_height());

				slice.transfer_scale_x *= f32(dst_width) / src_width;
				slice.transfer_scale_y *= f32(dst_height) / src_height;

				slice.width = src_width;
				slice.height = src_height;

				std::tie(slice.src_x, slice.src_y) = rsx::apply_resolution_scale<false>(src_res_scale, slice.src_x, slice.src_y, src_surface->get_surface_width(), src_surface->get_surface_height());
				std::tie(slice.dst_x, slice.dst_y) = rsx::apply_resolution_scale<false>(dst_res_scale, slice.dst_x, slice.dst_y, dst_surface->get_surface_width(), dst_surface->get_surface_height());
			}
		}

		template <typename T>
		surface_inheritance_result inherit_surface_contents(T* surface)
		{
			const auto child_w = get_surface_width<rsx::surface_metrics::bytes>();
			const auto child_h = get_surface_height<rsx::surface_metrics::bytes>();

			const auto parent_w = surface->template get_surface_width<rsx::surface_metrics::bytes>();
			const auto parent_h = surface->template get_surface_height<rsx::surface_metrics::bytes>();

			const auto [src_offset, dst_offset, size] = rsx::intersect_region(surface->base_addr, parent_w, parent_h, base_addr, child_w, child_h, get_rsx_pitch());

			if (!size.width || !size.height)
			{
				return surface_inheritance_result::none;
			}

			ensure(src_offset.x < parent_w && src_offset.y < parent_h);
			ensure(dst_offset.x < child_w && dst_offset.y < child_h);

			// TODO: Eventually need to stack all the overlapping regions, but for now just do the latest rect in the space
			deferred_clipped_region<T*> region{};
			region.src_x = src_offset.x;
			region.src_y = src_offset.y;
			region.dst_x = dst_offset.x;
			region.dst_y = dst_offset.y;
			region.width = size.width;
			region.height = size.height;
			region.source = surface;
			region.target = static_cast<T*>(this);

			set_old_contents_region(region, true);
			return (region.width == parent_w && region.height == parent_h) ?
				surface_inheritance_result::full :
				surface_inheritance_result::partial;
		}

		void on_write(u64 write_tag = 0,
			rsx::surface_state_flags resolve_flags = surface_state_flags::require_resolve,
			surface_raster_type type = rsx::surface_raster_type::undefined)
		{
			mark_content_written();

			if (write_tag)
			{
				// Update use tag if requested
				last_use_tag = write_tag;
			}

			// Tag unconditionally without introducing new data
			sync_tag();

			// HACK!! This should be cleared through memory barriers only
			state_flags = rsx::surface_state_flags::ready;

			if (spp > 1 && sample_layout != surface_sample_layout::null)
			{
				msaa_flags = resolve_flags;
			}

			if (!old_contents.empty())
			{
				clear_rw_barrier();
			}

			if (type != rsx::surface_raster_type::undefined)
			{
				raster_type = type;
			}
		}

		void on_write_copy(u64 write_tag = 0,
			bool keep_optimizations = false,
			surface_raster_type type = rsx::surface_raster_type::undefined)
		{
			on_write(write_tag, rsx::surface_state_flags::require_unresolve, type);

			if (!keep_optimizations && is_depth_surface())
			{
				// A successful write-copy occured, cannot guarantee flat contents in stencil area
				stencil_init_flags |= (1 << 9);
			}
		}

		inline void on_write_fast(u64 write_tag)
		{
			ensure(write_tag);
			mark_content_written();
			last_use_tag = write_tag;

			if (spp > 1 && sample_layout != surface_sample_layout::null)
			{
				msaa_flags |= rsx::surface_state_flags::require_resolve;
			}
		}

		// Returns the rect area occupied by this surface expressed as an 8bpp image with no AA
		inline areau get_normalized_memory_area() const
		{
			const u16 internal_width = get_surface_width<rsx::surface_metrics::bytes>();
			const u16 internal_height = get_surface_height<rsx::surface_metrics::bytes>();

			return { 0, 0, internal_width, internal_height };
		}

		inline rsx::address_range32 get_memory_range() const
		{
			return memory_range;
		}

		template <typename T>
		void transform_samples_to_pixels(area_base<T>& area)
		{
			if (spp == 1) [[likely]] return;

			area.x1 /= samples_x;
			area.x2 /= samples_x;
			area.y1 /= samples_y;
			area.y2 /= samples_y;
		}

		template <typename T>
		void transform_pixels_to_samples(area_base<T>& area)
		{
			if (spp == 1) [[likely]] return;

			area.x1 *= samples_x;
			area.x2 *= samples_x;
			area.y1 *= samples_y;
			area.y2 *= samples_y;
		}

		template <typename T>
		void transform_samples_to_pixels(T& x1, T& x2, T& y1, T& y2)
		{
			if (spp == 1) [[likely]] return;

			x1 /= samples_x;
			x2 /= samples_x;
			y1 /= samples_y;
			y2 /= samples_y;
		}

		template <typename T>
		void transform_pixels_to_samples(T& x1, T& x2, T& y1, T& y2)
		{
			if (spp == 1) [[likely]] return;

			x1 *= samples_x;
			x2 *= samples_x;
			y1 *= samples_y;
			y2 *= samples_y;
		}

		template<typename T>
		void transform_blit_coordinates(rsx::surface_access access_type, area_base<T>& region)
		{
			if (spp == 1 || sample_layout == rsx::surface_sample_layout::ps3)
				return;

			ensure(access_type.is_read() || access_type.is_transfer());
			transform_samples_to_pixels(region);
		}

		void on_lock()
		{
			add_ref();
			texture_cache_metadata.locked = true;
			texture_cache_metadata.timestamp = rsx::get_shared_tag();
		}

		void on_unlock()
		{
			texture_cache_metadata.locked = false;
			texture_cache_metadata.timestamp = rsx::get_shared_tag();
			release();
		}

		void on_swap_out()
		{
			if (is_locked())
			{
				on_unlock();
			}
			else
			{
				release();
			}
		}

		void on_swap_in(bool lock)
		{
			if (!is_locked() && lock)
			{
				on_lock();
			}
			else
			{
				add_ref();
			}
		}

		void on_clone_from(const render_target_descriptor* ref)
		{
			mark_content_written();

			if (ref->is_locked() && !is_locked())
			{
				// Propagate locked state only.
				texture_cache_metadata = ref->texture_cache_metadata;
			}

			rsx_pitch = ref->get_rsx_pitch();
			last_use_tag = ref->last_use_tag;
			raster_type = ref->raster_type;     // Can't actually cut up swizzled data
		}

		bool is_locked() const
		{
			return texture_cache_metadata.locked;
		}

		bool has_flushable_data() const
		{
			ensure(is_locked());
			ensure(texture_cache_metadata.timestamp);
			return (texture_cache_metadata.timestamp < last_use_tag);
		}
	};
}
