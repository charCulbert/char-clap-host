#include "gui.h"
#include "harness.h"
#include "native-window.h"

using nch::findEdgeStrips;
using nch::Image;
using nch::kHostBackground;

namespace {

// A grey image with the host's background painted over the given columns.
Image withBackgroundColumns(uint32_t width, uint32_t height, uint32_t from, uint32_t to) {
	Image image;
	image.width = width;
	image.height = height;
	image.rgb.assign(static_cast<size_t>(width) * height * 3, 0x80);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = from; x < to; ++x)
			for (int c = 0; c < 3; ++c)
				image.rgb[(static_cast<size_t>(y) * width + x) * 3 + c] = kHostBackground[c];
	return image;
}

} // namespace

TEST(an_interface_that_fills_its_window_has_no_strips) {
	const auto strips = findEdgeStrips(withBackgroundColumns(40, 30, 0, 0), kHostBackground);
	CHECK_EQ(strips.top + strips.bottom + strips.left + strips.right, 0u);
}

TEST(a_window_wider_than_its_interface_shows_a_strip_on_the_right) {
	const auto strips = findEdgeStrips(withBackgroundColumns(40, 30, 35, 40), kHostBackground);
	CHECK_EQ(strips.right, 5u);
	CHECK_EQ(strips.left, 0u);
	CHECK_EQ(strips.top, 0u);
	CHECK_EQ(strips.bottom, 0u);
}

TEST(a_rounded_corner_does_not_hide_a_strip) {
	// A few pixels of another colour in the strip, as a window corner gives.
	Image image = withBackgroundColumns(40, 30, 0, 3);
	for (int c = 0; c < 3; ++c)
		image.rgb[c] = 0;
	CHECK_EQ(findEdgeStrips(image, kHostBackground).left, 3u);
}

TEST(an_interface_that_drew_nothing_is_background_on_every_side) {
	const auto strips = findEdgeStrips(withBackgroundColumns(40, 30, 0, 40), kHostBackground);
	CHECK_EQ(strips.top, 30u);
	CHECK_EQ(strips.left, 40u);
}
