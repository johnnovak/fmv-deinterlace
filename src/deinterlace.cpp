#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int image_width;
int image_height;

// For storing RGBA pixel data
std::vector<uint32_t> input_image;

// Number of uint64_t's between two consecutive rows
int buffer_pitch;

// Number of uint64_t's before the start of the actual image data in each row
int buffer_offset = 2;

bool load_image(const char* filename)
{
	int channels_in_file;

	// Ask for RGBA pixels (uint32_t)
	constexpr int DesiredChannels = 4;

	uint8_t* data = stbi_load(filename,
	                          &image_width,
	                          &image_height,
	                          &channels_in_file,
	                          DesiredChannels);
	if (!data) {
		return false;
	}

	const auto num_pixels = image_width * image_height;
	input_image.resize(num_pixels);
	std::memcpy(input_image.data(), data, num_pixels * DesiredChannels);

	return true;
}

void threshold(std::vector<uint32_t>& src, std::vector<uint64_t>& dest)
{
	auto in       = src.data();
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto out = out_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			uint64_t out_buf = 0;

			// Build the 64-bit mask 8 pixels at a time to reduce
			// loop overhead.
			for (auto n = 0; n < 8; ++n) {
				// Make sure the alpha component is set to zero
				constexpr auto mask = 0x00ffffff;

				const auto a1 = in[0] & mask;
				const auto a2 = in[1] & mask;
				const auto a3 = in[2] & mask;
				const auto a4 = in[3] & mask;
				const auto a5 = in[4] & mask;
				const auto a6 = in[5] & mask;
				const auto a7 = in[6] & mask;
				const auto a8 = in[7] & mask;

				in += 8;

				// Non-black pixels are set to 1 in the bit
				// mask. We convert the pixels by row, top to
				// down, left to right. When converting the
				// first 64 pixels of a row, the LSB of the mask
				// uint64_t is the first pixel, and the MSB is
				// the 64th pixel.

				const uint8_t bits = ((a1 != 0) << 0) |
				                     ((a2 != 0) << 1) |
				                     ((a3 != 0) << 2) |
				                     ((a4 != 0) << 3) |
				                     ((a5 != 0) << 4) |
				                     ((a6 != 0) << 5) |
				                     ((a7 != 0) << 6) |
				                     ((a7 != 0) << 7);

				out_buf |= (uint64_t)bits << (n * 8);
			}
			*out = out_buf;
			++out;
		}

		out_line += buffer_pitch;
	}
}

void downshift_and_xor(std::vector<uint64_t>& src, std::vector<uint64_t>& dest)
{
	// Copy src into dest as a starting point (less than 1 us)
	dest = src;

	auto in_line = src.data() + buffer_offset + buffer_pitch;

	// Start writing from the second row
	auto out_line = dest.data() + buffer_offset + buffer_pitch * 2;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			*out ^= *in;
			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void dilate_horiz(std::vector<uint64_t>& src, std::vector<uint64_t>& dest)
{
	auto in_line  = src.data() + buffer_pitch + 1;
	auto out_line = dest.data() + buffer_pitch + 1;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		// We process the input horizontally in 64-pixel chunks.
		// This is the layout of a single chunk in an uint64_t:
		//
		//    bits         pixels
		//
		//    0-7    pixels N    to N+7
		//    8-15   pixels N+8  to N+15
		//   16-23   pixels N+16 to N+23
		//    ...            ...
		//   48-55   pixels N+48 to N+55
		//   56-63   pixels N+56 to N+63
		//
		uint64_t curr = *in++;
		uint64_t prev = 0;

		for (auto x = 0; x < image_width / 64 + 1; ++x) {
			const auto next = *in;
			++in;

			// "Shift in" the last pixel of the previous chunk
			const auto prev_pixel63 = (prev & ((uint64_t)1 << 63)) >> 63;
			const auto left_neighbours = (curr << 1) | prev_pixel63;

			// "Shift in" the firs pixel of the next chunk
			const auto next_pixel1      = (next & 1) << 63;
			const auto right_neighbours = next_pixel1 | curr >> 1;

			*out = left_neighbours | curr | right_neighbours;
			++out;

			prev = curr;
			curr = next;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void dilate_vert(std::vector<uint64_t>& src, std::vector<uint64_t>& dest)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			const auto prev = *(in - buffer_pitch);
			const auto curr = *in;
			const auto next = *(in + buffer_pitch);

			*out = prev | curr | next;

			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void erode_horiz(std::vector<uint64_t>& src, std::vector<uint64_t>& dest)
{
	auto in_line  = src.data() + buffer_pitch + 1;
	auto out_line = dest.data() + buffer_pitch + 1;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		// We process the input horizontally in 64-pixel chunks.
		// This is the layout of a single chunk in an uint64_t:
		//
		//    bits         pixels
		//
		//    0-7    pixels N    to N+7
		//    8-15   pixels N+8  to N+15
		//   16-23   pixels N+16 to N+23
		//    ...            ...
		//   48-55   pixels N+48 to N+55
		//   56-63   pixels N+56 to N+63
		//
		uint64_t curr = *in++;
		uint64_t prev = 0;

		for (auto x = 0; x < image_width / 64 + 1; ++x) {
			const auto next = *in;
			++in;

			// "Shift in" the last pixel of the previous chunk
			const auto prev_pixel63 = (prev & ((uint64_t)1 << 63)) >> 63;
			const auto left_neighbours = (curr << 1) | prev_pixel63;

			// "Shift in" the firs pixel of the next chunk
			const auto next_pixel1      = (next & 1) << 63;
			const auto right_neighbours = next_pixel1 | curr >> 1;

			*out = left_neighbours & curr & right_neighbours;
			++out;

			prev = curr;
			curr = next;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void erode_vert(std::vector<uint64_t>& src, std::vector<uint64_t>& dest)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			const auto prev = *(in - buffer_pitch);
			const auto curr = *in;
			const auto next = *(in + buffer_pitch);

			*out = prev & curr & next;

			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void deinterlace(std::vector<uint32_t>& src, std::vector<uint64_t>& mask,
                 std::vector<uint32_t>& dest)
{
	dest = src;

	auto in        = src.data();
	auto mask_line = mask.data() + buffer_offset + buffer_pitch * 2;
	auto out       = dest.data() + image_width;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto mask = mask_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			auto m = *mask;

			for (auto x = 0; x < 64; ++x) {
				if (m & 1) {
					const auto in_buf = *in;

					// Deinterlacing strength params
					//
					// low     1 / 2
					// medium  2 / 3
					// high    4 / 5
					// subtle  8 / 9
					// full    1 / 1

					uint64_t scaled = 0;
					scaled |=   (in_buf        & 0xff) * 8 / 9;
					scaled |= (((in_buf >> 8)  & 0xff) * 8 / 9) << 8;
					scaled |= (((in_buf >> 16) & 0xff) * 8 / 9) << 16;

					*out |= scaled;
				}
				m >>= 1;
				++out;
				++in;
			}
			++mask;
		}
		mask_line += buffer_pitch;
	}
}

#define WRITE_PASSES

void write_buffer(const char* filename, std::vector<uint64_t>& buf)
{
#ifdef WRITE_PASSES
	constexpr auto WriteComp = 1;

	auto in_line = buf.data() + buffer_offset + buffer_pitch;

	std::vector<uint8_t> out_buf(image_width * image_height);
	auto out = out_buf.data();

	for (auto y = 0; y < image_height; ++y) {
		auto in = in_line;

		for (auto x = 0; x < image_width / 64; ++x) {
			auto in_buf = *in;

			for (auto n = 0; n < 64; ++n) {
				*out = (in_buf & 1) ? 0xff : 0;
				++out;
				in_buf >>= 1;
			}
			++in;
		}
		in_line += buffer_pitch;
	}

	stbi_write_png(filename,
	               image_width,
	               image_height,
	               WriteComp,
	               out_buf.data(),
	               image_width);
#endif
}

int main(int argc, char* argv[])
{
	if (argc <= 1) {
		printf("Usage: deinterlace INPUT\n");
		exit(EXIT_FAILURE);
	}

	const auto input_file = argv[1];

	if (!load_image(input_file)) {
		fprintf(stderr, "Error loading image file '%s'\n", input_file);
		exit(EXIT_FAILURE);
	}

	assert(image_width % 8 == 0);

	// We store 64 1-bit pixels per uint64_t, plus 1 uint64_t for padding at
	// the end of each row. We also store two padding rows at the top and
	// bottom.
	const auto bufsize = (image_width / 64 + buffer_offset) * (image_height + 2);

	buffer_pitch = image_width / 64 + buffer_offset;

	// Fill buffers with zeroes
	std::vector<uint64_t> buffer1(bufsize, 0);
	std::vector<uint64_t> buffer2(bufsize, 0);
	std::vector<uint64_t> buffer3(bufsize, 0);

	std::vector<uint32_t> output_image(input_image.size());

	std::vector<uint64_t> durations_ns;

	constexpr auto NumIterations = 1;
//	constexpr auto NumIterations = 200;

	// for benchmarking
//	constexpr auto NumIterations = 200;

	// for benchmarking
//	srand(time(NULL));

	for (auto it = 0; it < NumIterations; ++it) {

		// for benchmarking
		// for (auto& x : input_image) {
		// 	x = rand();
		// }

		auto start = std::chrono::high_resolution_clock::now();
#if 1
		// 33 us
		threshold(input_image, buffer1);

		write_buffer("out/threshold.png", buffer1);

		// buffer 1 now contains the mask for the original image
		// (off for black pixels, on for non-black pixels)
#endif
#if 1
		// 1.51 us
		downshift_and_xor(buffer1, buffer2);

		write_buffer("out/downshift_and_xor.png", buffer2);
#endif
#if 1
		for (auto i = 0; i < 2; ++i) {
			// 1.92 us
			erode_horiz(buffer2, buffer3);

			// 1.44 us
			erode_vert(buffer3, buffer2);
		}
		// total 5.60 us

		write_buffer("out/erode.png", buffer2);
#endif
#if 1
		for (auto i = 0; i < 2; ++i) {
			// 1.92 us
			dilate_horiz(buffer2, buffer3);

			// 1.45 us
			dilate_vert(buffer3, buffer2);
		}
		// total 5.60 us

		write_buffer("out/dilate.png", buffer2);

		// buffer 2 now contains the mask for the interlaced FMV area
#endif
#if 1
		// 95 us
		deinterlace(input_image, buffer2, output_image);
#endif

		auto end = std::chrono::high_resolution_clock::now();
		uint64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

		durations_ns.emplace_back(nanoseconds);

#if 1
		constexpr auto WriteComp = 4;

		stbi_write_png("out/output.png",
		               image_width,
		               image_height,
		               WriteComp,
		               output_image.data(),
		               image_width * WriteComp);
#endif
	}

	// Benchmark results
	// =================
	// 10k iterations, averaged
	// 640x480 input image
	//
	//
	// 2024 MacMini, Apple M4
	// ----------------------
	//   uint8_t masks
	//       first implementation  1117 us
	//       threshold_8           1084 us
	//       downshift_and_xor_8   1058 us
	//       erode_vert_8           806 us
	//       dilate_vert_8          605 us
	//
	//  bitfield masks
	//       total                  155 us
	//
	//
	// AMD Ryzen 7900
	// --------------
	//   uint8_t masks
	//       first implementation  1454 us
	//
	//       threshold_8
	//       downshift_and_xor_8
	//       erode_vert_8
	//       dilate_vert_8          753 us
	//
	//  bitfield masks
	//       erode_horiz              5 us
	//       erode_vert             1.6 us
	//       dilate_horiz             5 us
	//       dilate_vert            1.6 us
	//       deinterlace             97 us
	//
	//       total                  220 us
	//

	double average_ns = 0;
	for (const auto t : durations_ns) {
		average_ns += (double)t;
	}
	average_ns /= (double)durations_ns.size();

	printf("Total time: %.2f microseconds\n", average_ns / 1000.0);
}

