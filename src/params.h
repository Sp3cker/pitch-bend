#pragma once

#include <cstdint>

// ─── Parameter IDs ────────────────────────────────────────────────────────────

enum class ParamId : uint32_t {
	GLIDE_TIME_MS    = 0,
	GLIDE_MODE       = 1,
	CURVE            = 2,
	LOOKAHEAD_MS     = 3,
	PITCH_BEND_RANGE = 4,
};

static constexpr uint32_t PARAM_COUNT = 5;

// ─── Typed enumerations ───────────────────────────────────────────────────────

enum class GlideMode : int {
	ALWAYS = 0,
	LEGATO = 1,
};

enum class CurveShape : int {
	LINEAR      = 0,
	EXPONENTIAL = 1,
	SIGMOID     = 2,
};

// ─── Static metadata table ────────────────────────────────────────────────────

struct param_info_t {
	ParamId     id;
	const char *name;
	double      min_value;
	double      max_value;
	double      default_value;
	bool        is_stepped;
};

static const param_info_t PARAM_INFO[PARAM_COUNT] = {
	{ ParamId::GLIDE_TIME_MS,    "Glide Time (ms)",    0.0,    2000.0,  80.0,  false },
	{ ParamId::GLIDE_MODE,       "Glide Mode",         0.0,    1.0,     1.0,   true  },
	{ ParamId::CURVE,            "Curve",              0.0,    2.0,     0.0,   true  },
	{ ParamId::LOOKAHEAD_MS,     "Lookahead (ms)",     0.0,    20.0,    8.0,   false },
	{ ParamId::PITCH_BEND_RANGE, "Pitch Bend Range",   1.0,    48.0,    12.0,  true  },
};
