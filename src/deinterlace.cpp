#include <cstdio>
#include <cstdlib>
#include <chrono>
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

		for (auto x = 0; x < image_width; ++x) {
			// Make sure alpha channel is set to zero
			auto a = *in & 0x00ffffff;

			// Create a mask where all non-black pixels are 0xff
			*out = (a > 0) ? 0xff : 0;

			++in;
			++out;
		}

		out_line += buffer_pitch;
	}
}

void downshift_and_xor(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	// Copy src into dest as a starting point
	dest = src;

	auto in_line = src.data() + buffer_offset + buffer_pitch;

	// Start writing from the second row
	auto out_line = dest.data() + buffer_offset + buffer_pitch * 2;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width; ++x) {
			*out ^= *in;
			++in;
			++out;
		}

		in_line += buffer_pitch;
		out_line += buffer_pitch;
	}
}

void downshift_and_xor_4(std::vector<uint8_t>& src, std::vector<uint8_t>& dest)
{
	// Copy src into dest as a starting point
	dest = src;

	auto in_line = src.data() + buffer_offset + buffer_pitch;

	// Start writing from the second row
	auto out_line = dest.data() + buffer_offset + buffer_pitch * 2;

	for (auto y = 0; y < (image_height - 1); ++y) {
		auto in  = in_line;
		auto out = out_line;

		for (auto x = 0; x < image_width / 8; ++x) {
			*((uint64_t *) out) ^= *(uint64_t *) in;
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
	dilate(src, dest, buffer_pitch);
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
	erode(src, dest, buffer_pitch);
}

void write_buffer(const char* filename, std::vector<uint8_t>& buf)
{
	constexpr auto WriteComp = 1;

	stbi_write_png(filename,
	               image_width,
	               image_height,
	               WriteComp,
	               buf.data() + buffer_offset + buffer_pitch,
	               buffer_pitch);
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

	const auto bufsize = (image_width + 2) * (image_height + 2);
	buffer_pitch       = image_width + 2;

	// Fill buffers with zeroes
	std::vector<uint8_t> buffer1(bufsize, 0);
	std::vector<uint8_t> buffer2(bufsize, 0);
	std::vector<uint8_t> buffer3(bufsize, 0);

	auto start = std::chrono::high_resolution_clock::now();

//	constexpr auto NumIterations = 1;
	constexpr auto NumIterations = 10000;
	for (auto it = 0; it < NumIterations; ++it) {

		// 80 us
//		threshold(input_image, buffer1);
		//	write_buffer("threshold.png", buffer1);

		// buffer 1 now contains the mask for the original image
		// (off for black pixels, on for non-black pixels)

		// uint84_ = 86 us
		// uint32_t = 30 us
		// uint64_t = 12 us
	//	downshift_and_xor(buffer1, buffer2);
//		downshift_and_xor_4(buffer1, buffer2);
//		write_buffer("downshift_and_xor.png", buffer2);

		// total 415 us
//		for (auto i = 0; i < 2; ++i) {
			// 105 us
//			erode_horiz(buffer2, buffer3);
//			// 105 us
			erode_vert(buffer3, buffer2);
//		}
		//	write_buffer("erode.png", buffer2);

		// total 418 us
//		for (auto i = 0; i < 2; ++i) {
			// total 208 us
	
			// 103 us
//			dilate_vert(buffer2, buffer3);
			// 105 us
//			dilate_horiz(buffer3, buffer2);
//		}
		//	write_buffer("dilate.png", buffer2);

		// buffer 2 now contains the mask for the interlaced FMV area
	}

	// TOTAL 1050 us
	//
	// after downshift_and_xor_4
	//   978

	auto end = std::chrono::high_resolution_clock::now();

	long long microseconds = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

	printf("Total time (microseconds): %lld\n", microseconds / NumIterations);
}

