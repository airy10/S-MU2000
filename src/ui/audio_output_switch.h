// license:BSD-3-Clause
// Reopen a standalone output without leaving a failed selection silent.
#pragma once

#include "audio_out.h"
#include "audio_preferences.h"
#include "audio_start.h"
#include <thread>

namespace ui {

struct audio_output_switch_result {
	bool selected = false;
	bool restored = false;
	std::string error;
	audio_output_config config;
};

template <typename Output, typename Fill>
audio_output_switch_result switch_audio_output(
	Output &out, const Fill &fill,
	const audio_output_config &wanted, const audio_output_config &previous, bool restore = true)
{
	audio_output_switch_result r;
	r.config = wanted;
	if (start_audio_stream(out, fill, r.config, r.error)) {
		r.selected = true;
		return r;
	}
	if (!restore) { out.stop(); return r; }
	std::string restore_error;
	r.config = previous;
	r.restored = start_audio_stream(out, fill, r.config, restore_error);
	if (!r.restored) {
		out.stop();
		r.error += "\n" + restore_error;
	}
	return r;
}

// Driver opening/closing can block. The UI owns the job and consumes its
// result only after joining; no backend getters may run while it is busy.
class audio_reopen_job {
public:
	~audio_reopen_job() { join(); }
	bool busy() const { return m_thread.joinable(); }
	bool done() const { return m_done.load(); }
	void start(std::function<audio_output_switch_result()> work)
	{
		m_done.store(false);
		m_thread = std::thread([this, work = std::move(work)] {
			m_result = work();
			m_done.store(true);
		});
	}
	audio_output_switch_result join()
	{
		if (m_thread.joinable()) m_thread.join();
		return m_result;
	}
private:
	std::thread m_thread;
	std::atomic<bool> m_done{false};
	audio_output_switch_result m_result;
};

inline audio_output_switch_result switch_audio_output(
	audio_out &out, int latency_ms, const audio_out::fill_fn &fill,
	bool exclusive, const std::string &wanted, const std::string &previous)
{
	audio_output_config next, old;
	next.device = wanted;
	old.device = previous;
	next.preferences.latency_ms = old.preferences.latency_ms = latency_ms;
	next.preferences.exclusive = old.preferences.exclusive = exclusive;
	return switch_audio_output(out, fill, next, old);
}

} // namespace ui
