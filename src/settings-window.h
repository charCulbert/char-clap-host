// The host's own interface: a device selector in a host-owned window.
//
// It is built on exactly the same webview the host uses for a plug-in's
// clap.webview interface, and on the same device layer the `audio.*` and
// `midi.*` commands use, so the window and the prompt can never disagree
// about what is selected.
#pragma once

#include "device-settings.h"
#include "json.h"
#include "webview.h"

#include <memory>
#include <string>

namespace nch {

class NativeWindow;
class Session;

class SettingsWindow {
public:
	explicit SettingsWindow(Session &session);
	~SettingsWindow();
	SettingsWindow(const SettingsWindow &) = delete;
	SettingsWindow &operator=(const SettingsWindow &) = delete;

	bool open(std::string &error);
	void close();
	bool isOpen() const;
	// True once the user closed the window, so the main loop can tidy up.
	bool wantsClose() const;

	// The device state the interface shows, in the shape Compost's device
	// selector expects.
	Value snapshot() const;
	// Says the host has opened every MIDI input, so the interface shows the
	// "all devices" choice ticked and a port appearing later is followed too.
	void followAllMidiInputs(bool follow) { followAllMidiInputs_ = follow; }
	// What the host's devices currently are, which the decision is made from.
	DeviceState deviceState() const;
	// Applies a change from the interface. Returns the resulting snapshot.
	Value apply(const Value &request, std::string &error);

private:
	std::optional<WebviewHost::Resource> fetch(const std::string &path) const;
	void onMessage(const uint8_t *bytes, uint32_t size);
	void sendSnapshot();

	Session &session_;
	std::unique_ptr<NativeWindow> window_;
	WebviewHost webview_;
	// Whether the user asked for every MIDI input, which is a decision and not
	// something to infer from the ports that happen to be open: with one port
	// on the system, "all of them" and "that one" are the same set.
	bool followAllMidiInputs_ = false;
};

} // namespace nch
