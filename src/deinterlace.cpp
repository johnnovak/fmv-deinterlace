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

// Number of bytes between two consecutive rows
int buffer_pitch;

// Number of bytes before the start of the actual image data in each row
int buffer_offset = 1;

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

void threshold(std::vector<uint32_t>& src, std::vector<uint8_t>& dest)
{
	auto in       = src.data();
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto out = out_line;

		for (auto x = 0; x < image_width / (8 * 8); ++x) {
			uint64_t out_buf = 0;

			for (auto buf_pos = 0; buf_pos < 8; ++buf_pos) {
				uint8_t m = 0;
				out_buf >>= 8;

				for (auto n = 0; n < 8; ++n) {
					// Make sure the alpha component is set
					// to zero
					const auto a = *in & 0x00ffffff;
					++in;

					// Non-black pixels are set to 1 in the
					// bit mask
					m |= (a > 0) ? (1 << n) : 0;
				}
				out_buf |= ((uint64_t)m) << 56;
			}

			*((uint64_t*)out) = out_buf;
			out += 8;
		}

		out_line += buffer_pitch;
	}
}

void downshift_and_xor(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	// Copy src into dest as a starting point (less than 1 us)
	dest = src;

	auto in_line = src.data() + buffer_offset + buffer_pitch;

	// Start writing from the second row
	auto out_line = dest.data() + buffer_offset + buffer_pitch * 2;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / (8 * 8); ++x) {
			*((uint64_t*)out) ^= *(uint64_t*)in;
			in += 8;
			out += 8;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void dilate(std::vector<uint8_t>& src, std::vector<uint8_t>& dest, int neighbour_offset)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width; ++x) {
			const auto prev = *(in - neighbour_offset);
			const auto curr = *in;
			const auto next = *(in + neighbour_offset);

			*out = prev | curr | next;

			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void dilate_horiz(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	dilate(src, dest, 1);
}

void dilate_vert(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / (8 * 8); ++x) {
			const auto prev = *((uint64_t*)(in - buffer_pitch));
			const auto curr = *((uint64_t*)in);
			const auto next = *((uint64_t*)(in + buffer_pitch));

			*((uint64_t*)out) = prev | curr | next;

			in += 8;
			out += 8;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void erode(std::vector<uint8_t>& src, std::vector<uint8_t>& dest, int neighbour_offset)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width; ++x) {
			const auto prev = *(in - neighbour_offset);
			const auto curr = *in;
			const auto next = *(in + neighbour_offset);

			*out = prev & curr & next;

			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void erode_horiz(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	erode(src, dest, 1);
}

void erode_vert(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	auto in_line  = src.data() + buffer_offset + buffer_pitch;
	auto out_line = dest.data() + buffer_offset + buffer_pitch;

	for (auto y = 0; y < image_height; ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / (8 * 8); ++x) {
			const auto prev = *((uint64_t*)(in - buffer_pitch));
			const auto curr = *((uint64_t*)in);
			const auto next = *((uint64_t*)(in + buffer_pitch));

			*((uint64_t*)out) = prev & curr & next;

			in += 8;
			out += 8;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void deinterlace(std::vector<uint32_t>& src, std::vector<uint8_t>& mask,
                 std::vector<uint32_t>& dest)
{
	dest = src;

	auto in        = src.data();
	auto mask_line = mask.data() + buffer_offset + buffer_pitch * 2;
	auto out       = dest.data() + image_width;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto mask = mask_line;

		for (auto x = 0; x < image_width; ++x) {
			if (*mask) {
				*out |= *in;
			}

			++out;
			++in;
			++mask;
		}

		mask_line += buffer_pitch;
	}
}

#define WRITE_PASSES

void write_buffer(const char* filename, std::vector<uint8_t>& buf)
{
#ifdef WRITE_PASSES
	constexpr auto WriteComp = 1;

	auto in_line = buf.data() + buffer_offset + buffer_pitch;

	std::vector<uint8_t> out_buf(image_width * image_height);
	auto out = out_buf.data();

	for (auto y = 0; y < image_height; ++y) {
		auto in = in_line;

		for (auto x = 0; x < image_width / 8; ++x) {
			auto p = *in;

			for (auto n = 0; n < 8; ++n) {
				*out = (p & 1) ? 0xff : 0;
				++out;
				p >>= 1;
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

	// We store eight 1-bit pixels per byte
	const auto bufsize = (image_width / 8 + 2) * (image_height + 2);
	buffer_pitch       = image_width / 8 + 2;

	// Fill buffers with zeroes
	std::vector<uint8_t> buffer1(bufsize, 0);
	std::vector<uint8_t> buffer2(bufsize, 0);
	std::vector<uint8_t> buffer3(bufsize, 0);

	std::vector<uint32_t> output_image(input_image.size());

	std::vector<uint64_t> durations_ns;

//	constexpr auto NumIterations = 1;
	constexpr auto NumIterations = 200;

	srand(time(NULL));

	for (auto it = 0; it < NumIterations; ++it) {

		for (auto& x : input_image) {
			x = rand();
		}

		auto start = std::chrono::high_resolution_clock::now();
#if 1
		// 33 us
//		threshold(input_image, buffer1);

//		write_buffer("threshold.png", buffer1);

		// buffer 1 now contains the mask for the original image
		// (off for black pixels, on for non-black pixels)
#endif
#if 1
		// 1.51 us
		downshift_and_xor(buffer1, buffer2);

//		write_buffer("downshift_and_xor.png", buffer2);
#endif
#if 0
		for (auto i = 0; i < 2; ++i) {
			// 107 us
			erode_horiz(buffer2, buffer3);

			// 105 us
			// erode_vert(buffer3, buffer2);

			// 7 us
			erode_vert_8(buffer3, buffer2);
		}
		// total (erode_vert)    418 us
		// total (erode_vert_8)  220 us

		write_buffer("erode.png", buffer2);
#endif
#if 0
		for (auto i = 0; i < 2; ++i) {
			// 107 us
			dilate_horiz(buffer2, buffer3);

			// 105 us
			// dilate_vert(buffer3, buffer2);

			// 7 us
			dilate_vert_8(buffer3, buffer2);
		}
		// total (dilate_vert)    420 us
		// total (dilate_vert_8)  220 us

		write_buffer("dilate.png", buffer2);

		// buffer 2 now contains the mask for the interlaced FMV area
#endif
#if 0
		// 95 us
		deinterlace(input_image, buffer2, output_image);
#endif

		auto end = std::chrono::high_resolution_clock::now();
		uint64_t nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

		durations_ns.emplace_back(nanoseconds);

#if 1
		constexpr auto WriteComp = 4;

		stbi_write_png("output.png",
		               image_width,
		               image_height,
		               WriteComp,
		               output_image.data(),
		               image_width * WriteComp);
#endif
	}

	// Benchmark results
	// -----------------
	// 10k iterations, averaged
	// 640x480 input image
	//
	//
	// 2024 MacMini, Apple M4
	//
	//   uint8_t masks
	//       first implementation  1117 us
	//       threshold_8           1084 us
	//       downshift_and_xor_8   1058 us
	//       erode_vert_8           806 us
	//       dilate_vert_8          605 us

	
	double average_ns = 0;
	for (const auto t : durations_ns) {
		average_ns += (double) t;
	}
	average_ns /= (double) durations_ns.size();

	printf("Total time: %.2f microseconds\n", average_ns / 1000.0);
}

