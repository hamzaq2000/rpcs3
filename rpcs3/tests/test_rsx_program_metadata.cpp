#include <gtest/gtest.h>

#include "Emu/RSX/Program/ProgramStateCache.h"

#include <array>
#include <limits>
#include <vector>

namespace
{
	using namespace rsx::assembler;
	using fragment_program_utils = program_hash_util::fragment_program_utils;

	constexpr u32 encode_fragment_program_word(u32 word)
	{
		return ((word & 0x00FF00FF) << 8) |
			((word & 0xFF00FF00) >> 8);
	}

	std::array<u32, 4> make_texture_instruction(
		FP_opcode opcode,
		u32 texture,
		register_type source_type,
		u32 input_attribute = 0,
		bool indexed_input = false,
		bool end = false,
		u32 swizzle_x = 0,
		u32 swizzle_y = 1,
		bool absolute = false,
		bool negate = false,
		u32 source_precision = RSX_FP_PRECISION_REAL)
	{
		OPDEST dst{};
		dst.opcode = opcode & 0x3f;
		dst.tex_num = texture;
		dst.src_attr_reg_num = input_attribute;
		dst.end = end;

		SRC0 src0{};
		src0.reg_type = source_type;
		src0.swizzle_x = swizzle_x;
		src0.swizzle_y = swizzle_y;
		src0.swizzle_z = 2;
		src0.swizzle_w = 3;
		src0.abs = absolute;
		src0.neg = negate;

		SRC1 src1{};
		src1.opcode_hi = opcode >> 6;
		src1.src0_prec_mod = source_precision;

		SRC2 src2{};
		src2.use_index_reg = indexed_input;

		return
		{
			encode_fragment_program_word(dst.HEX),
			encode_fragment_program_word(src0.HEX),
			encode_fragment_program_word(src1.HEX),
			encode_fragment_program_word(src2.HEX),
		};
	}

	void append_instruction(std::vector<u32>& program, const std::array<u32, 4>& instruction)
	{
		program.insert(program.end(), instruction.begin(), instruction.end());
	}

	TEST(TestFragmentProgramMetadata, TextureOpcodeClassesAndCounts)
	{
		using opcode_class = fragment_program_utils::texture_opcode_class;

		std::vector<u32> program;
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 3, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 3, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TXP, 4, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TXD, 5, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TXB, 6, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TXL, 7, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEXBEM, 8, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TXPBEM, 9, RSX_FP_REGISTER_TYPE_TEMP, 0, false, true));

		const auto metadata = fragment_program_utils::analyse_fragment_program(program.data());
		const auto mask = [](opcode_class value) { return static_cast<u16>(value); };

		EXPECT_EQ(metadata.referenced_textures_mask, 0x3f8);
		EXPECT_EQ(metadata.texture_instructions[3].opcode_class_mask, mask(opcode_class::plain));
		EXPECT_EQ(metadata.texture_instructions[3].instruction_count, 2);
		EXPECT_EQ(metadata.texture_instructions[4].opcode_class_mask, mask(opcode_class::projected));
		EXPECT_EQ(metadata.texture_instructions[5].opcode_class_mask, mask(opcode_class::gradients));
		EXPECT_EQ(metadata.texture_instructions[6].opcode_class_mask, mask(opcode_class::bias));
		EXPECT_EQ(metadata.texture_instructions[7].opcode_class_mask, mask(opcode_class::explicit_lod));
		EXPECT_EQ(metadata.texture_instructions[8].opcode_class_mask, mask(opcode_class::plain) | mask(opcode_class::bump_env));
		EXPECT_EQ(metadata.texture_instructions[9].opcode_class_mask, mask(opcode_class::projected) | mask(opcode_class::bump_env));
		for (u32 texture = 4; texture <= 9; texture++)
		{
			EXPECT_EQ(metadata.texture_instructions[texture].instruction_count, 1);
		}
	}

	TEST(TestFragmentProgramMetadata, DirectTextureCoordinateSources)
	{
		using source = fragment_program_utils::texture_coordinate_source;

		std::vector<u32> program;
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 0, RSX_FP_REGISTER_TYPE_INPUT, 0));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 1, RSX_FP_REGISTER_TYPE_INPUT, 4));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 2, RSX_FP_REGISTER_TYPE_INPUT, 14));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 3, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 4, RSX_FP_REGISTER_TYPE_CONSTANT));
		program.insert(program.end(), 4, 0); // Literal constant payload.
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 5, RSX_FP_REGISTER_TYPE_UNKNOWN));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 6, RSX_FP_REGISTER_TYPE_INPUT, 5, true));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 7, RSX_FP_REGISTER_TYPE_INPUT, 0));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 7, RSX_FP_REGISTER_TYPE_TEMP));
		append_instruction(program, make_texture_instruction(RSX_FP_OPCODE_TEX, 8, RSX_FP_REGISTER_TYPE_INPUT, 0, false, true));

		const auto metadata = fragment_program_utils::analyse_fragment_program(program.data());
		const auto mask = [](source value) { return static_cast<u8>(value); };

		EXPECT_EQ(metadata.texture_instructions[0].direct_coordinate_source_mask, mask(source::wpos));
		EXPECT_EQ(metadata.texture_instructions[1].direct_coordinate_source_mask, mask(source::texcoord));
		EXPECT_EQ(metadata.texture_instructions[2].direct_coordinate_source_mask, mask(source::other_input));
		EXPECT_EQ(metadata.texture_instructions[3].direct_coordinate_source_mask, mask(source::temporary));
		EXPECT_EQ(metadata.texture_instructions[4].direct_coordinate_source_mask, mask(source::constant));
		EXPECT_EQ(metadata.texture_instructions[5].direct_coordinate_source_mask, mask(source::unknown));
		EXPECT_EQ(metadata.texture_instructions[6].direct_coordinate_source_mask, mask(source::unknown));
		EXPECT_TRUE(metadata.texture_instructions[6].has_indexed_input);
		EXPECT_EQ(metadata.texture_instructions[7].direct_coordinate_source_mask, mask(source::wpos) | mask(source::temporary));
		EXPECT_TRUE(metadata.texture_instructions[7].has_mixed_coordinate_sources);
		EXPECT_FALSE(metadata.texture_instructions[8].has_mixed_coordinate_sources);
		EXPECT_FALSE(metadata.texture_instructions[8].has_non_identity_coordinate_expression);
		EXPECT_EQ(metadata.program_constants_buffer_length, 16);

		const auto& unused = metadata.texture_instructions[15];
		EXPECT_EQ(unused.opcode_class_mask, 0);
		EXPECT_EQ(unused.instruction_count, 0);
		EXPECT_EQ(unused.direct_coordinate_source_mask, 0);
		EXPECT_FALSE(unused.has_indexed_input);
		EXPECT_FALSE(unused.has_mixed_coordinate_sources);
		EXPECT_FALSE(unused.has_non_identity_coordinate_expression);
	}

	TEST(TestFragmentProgramMetadata, TextureCoordinateExpressionModifiers)
	{
		std::vector<u32> program;
		append_instruction(program, make_texture_instruction(
			RSX_FP_OPCODE_TEX, 0, RSX_FP_REGISTER_TYPE_INPUT, 0));
		append_instruction(program, make_texture_instruction(
			RSX_FP_OPCODE_TEX, 1, RSX_FP_REGISTER_TYPE_INPUT, 0, false, false, 1, 0));
		append_instruction(program, make_texture_instruction(
			RSX_FP_OPCODE_TEX, 2, RSX_FP_REGISTER_TYPE_INPUT, 0, false, false, 0, 1, true));
		append_instruction(program, make_texture_instruction(
			RSX_FP_OPCODE_TEX, 3, RSX_FP_REGISTER_TYPE_INPUT, 0, false, false, 0, 1, false, true));
		append_instruction(program, make_texture_instruction(
			RSX_FP_OPCODE_TEX, 4, RSX_FP_REGISTER_TYPE_INPUT, 0, false, true, 0, 1, false, false,
			RSX_FP_PRECISION_HALF));

		const auto metadata = fragment_program_utils::analyse_fragment_program(program.data());
		EXPECT_FALSE(metadata.texture_instructions[0].has_non_identity_coordinate_expression);
		for (u32 texture = 1; texture <= 4; texture++)
		{
			EXPECT_TRUE(metadata.texture_instructions[texture].has_non_identity_coordinate_expression);
		}
	}

	TEST(TestFragmentProgramMetadata, TextureInstructionCountSaturates)
	{
		constexpr u32 instruction_count = std::numeric_limits<u16>::max() + 1u;
		std::vector<u32> program;
		program.reserve(instruction_count * 4);

		for (u32 index = 0; index < instruction_count; index++)
		{
			append_instruction(program, make_texture_instruction(
				RSX_FP_OPCODE_TEX,
				0,
				RSX_FP_REGISTER_TYPE_TEMP,
				0,
				false,
				index + 1 == instruction_count));
		}

		const auto metadata = fragment_program_utils::analyse_fragment_program(program.data());
		EXPECT_EQ(metadata.texture_instructions[0].instruction_count, std::numeric_limits<u16>::max());
	}
}
