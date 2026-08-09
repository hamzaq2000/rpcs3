#pragma once

#include "util/types.hpp"

#include <cstdlib>

namespace rsx
{
	enum class framebuffer_feedback_copy_reason : u8
	{
		none,
		live_rop,
		edge_clamped_merge,
	};

	// Debug-overlay attribution for a deferred framebuffer-feedback request.
	// This is deliberately descriptive only; no renderer decision depends on it.
	enum class framebuffer_feedback_oracle_outcome : u8
	{
		none,
		new_snapshot,
		refresh,
		generation_reuse,
		static_hit,
		failure,
	};

	// The architecture oracle is deliberately opt-in: an ordinary debug-overlay
	// session must not pay for per-request identities, grouping, or trace markers.
	inline bool framebuffer_feedback_oracle_enabled(bool debug_overlay) noexcept
	{
		static const bool requested = []
		{
			const char* value = std::getenv("RPCS3_FRAMEBUFFER_FEEDBACK_ORACLE");
			return value && value[0] && value[0] != '0';
		}();

		return requested && debug_overlay;
	}
}
