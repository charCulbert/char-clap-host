#include "web-assets.h"

#include <choc/text/choc_Files.h>

#include <filesystem>

namespace nch {
namespace {

std::string mimeForPath(const std::string &path) {
	const auto dot = path.find_last_of('.');
	const std::string extension = dot == std::string::npos ? "" : path.substr(dot);
	if (extension == ".js" || extension == ".mjs")
		return "text/javascript";
	if (extension == ".css")
		return "text/css";
	if (extension == ".html")
		return "text/html";
	if (extension == ".json")
		return "application/json";
	if (extension == ".svg")
		return "image/svg+xml";
	return "application/octet-stream";
}

} // namespace

std::optional<WebviewHost::Resource> compostResource(const std::string &path) {
	const std::string prefix = "/compost/";
	if (path.rfind(prefix, 0) != 0 || path.find("..") != std::string::npos)
		return {};
	const std::filesystem::path file =
	    std::filesystem::path(NCH_COMPOST_ROOT) / "src" / path.substr(prefix.size());
	WebviewHost::Resource resource;
	try {
		choc::file::readFileContent(file, [&resource](uint64_t size) {
			resource.data.resize(static_cast<size_t>(size));
			return static_cast<void *>(resource.data.data());
		});
	} catch (const std::exception &) {
		return {};
	}
	resource.mimeType = mimeForPath(path);
	return resource;
}

WebviewHost::Resource htmlResource(const std::string &html) {
	WebviewHost::Resource resource;
	resource.data.assign(html.begin(), html.end());
	resource.mimeType = "text/html";
	return resource;
}

} // namespace nch
