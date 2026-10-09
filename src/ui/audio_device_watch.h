// license:BSD-3-Clause
#pragma once
#include <string>
#include <vector>

namespace ui {
// A failed reopen may be retried once per device-list/default change.
class audio_device_watch {
public:
	bool changed(const std::vector<std::string> &outputs, const std::string &default_name)
	{
		const bool change = !m_seen || outputs != m_outputs || default_name != m_default;
		m_seen = true; m_outputs = outputs; m_default = default_name;
		return change;
	}
private:
	bool m_seen = false;
	std::vector<std::string> m_outputs;
	std::string m_default;
};
} // namespace ui
