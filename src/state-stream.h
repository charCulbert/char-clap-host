// clap_istream / clap_ostream over a byte vector.
//
// Plug-in state is opaque, so the host only ever moves bytes; keeping that in
// one place means every state path (state, state-context, presets) shares the
// same short-read and short-write handling.
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nch {

class OutputStream {
public:
	OutputStream() {
		stream_.ctx = this;
		stream_.write = write;
	}

	const clap_ostream_t *stream() const { return &stream_; }
	const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
	static int64_t write(const clap_ostream_t *stream, const void *buffer, uint64_t size) {
		auto *self = static_cast<OutputStream *>(stream->ctx);
		const auto *data = static_cast<const uint8_t *>(buffer);
		self->bytes_.insert(self->bytes_.end(), data, data + size);
		return static_cast<int64_t>(size);
	}

	clap_ostream_t stream_{};
	std::vector<uint8_t> bytes_;
};

class InputStream {
public:
	explicit InputStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {
		stream_.ctx = this;
		stream_.read = read;
	}

	const clap_istream_t *stream() const { return &stream_; }
	uint64_t remaining() const { return bytes_.size() - position_; }

private:
	static int64_t read(const clap_istream_t *stream, void *buffer, uint64_t size) {
		auto *self = static_cast<InputStream *>(stream->ctx);
		const uint64_t available = self->bytes_.size() - self->position_;
		const uint64_t count = size < available ? size : available;
		if (count != 0)
			std::memcpy(buffer, self->bytes_.data() + self->position_, count);
		self->position_ += count;
		return static_cast<int64_t>(count);
	}

	clap_istream_t stream_{};
	std::vector<uint8_t> bytes_;
	uint64_t position_ = 0;
};

// Whole-file helpers for the state commands.
bool readAllBytes(const std::string &path, std::vector<uint8_t> &out, std::string &error);
bool writeAllBytes(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error);

} // namespace nch
