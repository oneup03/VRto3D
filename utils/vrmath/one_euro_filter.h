// one_euro_filter.h — Speed-adaptive low-pass filter
// Based on: "1 Euro Filter" by Casiez, Roussel, Vogel (CHI 2012)
// Original BSD 3-Clause License (Casiez/Roussel 2019) — see utils/LICENSE
// Adapted: header-only, float, value types, no exceptions
#pragma once
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class LowPassFilter {
    float y_ = 0.0f, s_ = 0.0f, a_ = 0.5f;
    bool initialized_ = false;

public:
    LowPassFilter() = default;
    explicit LowPassFilter(float alpha, float initval = 0.0f)
        : y_(initval), s_(initval)
        , a_(alpha < 0.0001f ? 0.0001f : (alpha > 1.0f ? 1.0f : alpha)) {}

    float filter(float value) {
        if (initialized_) {
            s_ = a_ * value + (1.0f - a_) * s_;
        } else {
            s_ = value;
            initialized_ = true;
        }
        y_ = value;
        return s_;
    }

    float filterWithAlpha(float value, float alpha) {
        a_ = (alpha < 0.0001f) ? 0.0001f : (alpha > 1.0f ? 1.0f : alpha);
        return filter(value);
    }

    bool hasLastRawValue() const { return initialized_; }
    float lastRawValue() const { return y_; }
    float lastFilteredValue() const { return s_; }
    void reset() { initialized_ = false; y_ = 0.0f; s_ = 0.0f; }
};

class OneEuroFilter {
    float freq_, mincutoff_, beta_, dcutoff_;
    LowPassFilter x_, dx_;
    float lasttime_ = -1.0f;

    float alpha(float cutoff) const {
        float te = 1.0f / freq_;
        float tau = 1.0f / (2.0f * static_cast<float>(M_PI) * cutoff);
        return 1.0f / (1.0f + tau / te);
    }

public:
    OneEuroFilter() : freq_(60.0f), mincutoff_(1.0f), beta_(0.0f), dcutoff_(1.0f) {
        x_ = LowPassFilter(alpha(mincutoff_));
        dx_ = LowPassFilter(alpha(dcutoff_));
    }

    OneEuroFilter(float freq, float mincutoff = 1.0f, float beta = 0.0f, float dcutoff = 1.0f)
        : freq_(freq > 0.0f ? freq : 60.0f)
        , mincutoff_(mincutoff > 0.0f ? mincutoff : 1.0f)
        , beta_(beta)
        , dcutoff_(dcutoff > 0.0f ? dcutoff : 1.0f)
    {
        x_ = LowPassFilter(alpha(mincutoff_));
        dx_ = LowPassFilter(alpha(dcutoff_));
    }

    float filter(float value, float timestamp = -1.0f) {
        if (lasttime_ >= 0.0f && timestamp >= 0.0f && timestamp > lasttime_) {
            float dt = timestamp - lasttime_;
            if (dt > 0.0f) freq_ = 1.0f / dt;
        }
        lasttime_ = timestamp;

        float dvalue = x_.hasLastRawValue()
            ? (value - x_.lastFilteredValue()) * freq_
            : 0.0f;
        float edvalue = dx_.filterWithAlpha(dvalue, alpha(dcutoff_));

        float cutoff = mincutoff_ + beta_ * std::fabs(edvalue);

        return x_.filterWithAlpha(value, alpha(cutoff));
    }

    void setFrequency(float f) { if (f > 0.0f) freq_ = f; }
    void setMinCutoff(float mc) { if (mc > 0.0f) mincutoff_ = mc; }
    void setBeta(float b) { beta_ = b; }
    float getMinCutoff() const { return mincutoff_; }
    float getBeta() const { return beta_; }
    float getFrequency() const { return freq_; }
    void reset() { x_.reset(); dx_.reset(); lasttime_ = -1.0f; }
};
