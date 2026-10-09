// license:BSD-3-Clause
#pragma once
#include <algorithm>
#include <cmath>

namespace ui {
// Stereo-linked sample-peak limiting, after panel gain and before PCM clipping.
// No lookahead; this does not claim true-peak/inter-sample limiting.
class output_limiter {
public:
	void reset() { m_gain = 1; }
	void process(double &left, double &right, bool enabled)
	{
		if (!enabled) { m_gain = 1; return; }
		if (!std::isfinite(left)) left = 0;
		if (!std::isfinite(right)) right = 0;
		const double peak = std::max(std::abs(left), std::abs(right));
		const double target = peak > 0.98 ? 0.98 / peak : 1;
		m_gain = std::min(target, 1 - (1 - m_gain) * m_release);
		left *= m_gain;
		right *= m_gain;
	}
private:
	double m_gain = 1;
	const double m_release = std::exp(-1.0 / (0.1 * 44100));
};
} // namespace ui
