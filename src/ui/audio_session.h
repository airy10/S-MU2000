// license:BSD-3-Clause
#pragma once
#include "audio_output_switch.h"
#include "engine.h"
#include <optional>

namespace ui {
// Reopening an output is independent of firmware boot/reset. In particular,
// losing every output does not turn valid emulated RAM into a boot failure.
class audio_session {
public:
	bool busy() const { return m_job.busy(); }
	void begin(engine &eng, audio_out &out, audio_output_config wanted, audio_output_config previous, bool restore = true)
	{
		m_engine = &eng;
		eng.state.store(0);
		m_job.start([&eng, &out, wanted = std::move(wanted), previous = std::move(previous), restore] {
			while (eng.in_fill.load()) smu2000::sleep_ms(1);
#if defined(__APPLE__)
			const bool threaded = eng.mu.threading_requested();
			eng.mu.set_threaded(false);
			eng.mu.set_realtime_workgroup(nullptr);
#endif
			const auto result = switch_audio_output(out, [&eng](s16 *dst, u32 n) { eng.fill(dst, n); }, wanted, previous, restore);
#if defined(__APPLE__)
			if (result.selected || result.restored) eng.mu.set_realtime_workgroup(out.realtime_workgroup());
			eng.mu.set_threaded(threaded);
#endif
			return result;
		});
	}
	std::optional<audio_output_switch_result> poll()
	{
		if (!busy() || !m_job.done()) return std::nullopt;
		return finish();
	}
	std::optional<audio_output_switch_result> join()
	{
		if (!busy()) return std::nullopt;
		return finish();
	}
private:
	audio_output_switch_result finish()
	{
		auto result = m_job.join();
		m_engine->state.store(result.selected || result.restored ? 1 : 2);
		return result;
	}
	engine *m_engine = nullptr;
	audio_reopen_job m_job;
};
} // namespace ui
