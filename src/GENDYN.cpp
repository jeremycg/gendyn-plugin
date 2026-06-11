#include "plugin.hpp"
#include <algorithm>
#include <cmath>

static const int MAX_N = 64;

struct GENDYN : Module {

    enum ParamId {
        N_PARAM,
        SCALE_AMP_PARAM,
        SCALE_DUR_PARAM,
        B_AMP_PARAM,
        B_DUR_CENTER_PARAM,
        B_DUR_WIDTH_PARAM,
        DISTRIBUTION_PARAM,
        SCALE_AMP_ATT_PARAM,
        SCALE_DUR_ATT_PARAM,
        B_AMP_ATT_PARAM,
        B_DUR_ATT_PARAM,
        PARAMS_LEN
    };

    enum InputId {
        SCALE_AMP_INPUT,
        SCALE_DUR_INPUT,
        B_AMP_INPUT,
        B_DUR_INPUT,
        INPUTS_LEN
    };

    enum OutputId {
        AUDIO_OUTPUT,
        CYCLE_TRIG_OUTPUT,
        FREQ_CV_OUTPUT,
        OUTPUTS_LEN
    };

    enum LightId {
        LIGHTS_LEN
    };

    float amp[MAX_N];               // breakpoint amplitudes (volts, range ±bAmp)
    float dur[MAX_N];               // breakpoint durations  (samples)
    int   current_breakpoint = 0;
    int   current_sample     = 0;
    float current_amp        = 0.f; // amplitude at the start of the current segment
    float target_amp         = 0.f; // amplitude at the end   of the current segment
    int   current_dur        = 1;   // length of the current segment in samples
    int   last_N             = -1;  // -1 forces reinit on the first process() call

    dsp::PulseGenerator cycleTrigger;

    GENDYN() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(N_PARAM,            2.f,   64.f,   13.f,  "Breakpoints (N)");
        getParamQuantity(N_PARAM)->snapEnabled = true;

        configParam(SCALE_AMP_PARAM,    0.f,   1.f,    0.006f,"Amplitude step scale");
        configParam(SCALE_DUR_PARAM,    0.f,   1.f,    0.01f, "Duration step scale");
        configParam(B_AMP_PARAM,        0.f,   1.f,    0.8f,  "Amplitude barrier");
        configParam(B_DUR_CENTER_PARAM, 20.f,  5000.f, 220.f, "Center frequency", " Hz");
        configParam(B_DUR_WIDTH_PARAM,  0.f,   1.f,    0.4f,  "Frequency barrier width");

        configParam(DISTRIBUTION_PARAM, 0.f,   3.f,    3.f,
            "Distribution (0=Cauchy  1=Gauss  2=Uniform  3=Logistic)");
        getParamQuantity(DISTRIBUTION_PARAM)->snapEnabled = true;

        configParam(SCALE_AMP_ATT_PARAM,-1.f,  1.f,    0.f,   "Scale Amp attenuverter");
        configParam(SCALE_DUR_ATT_PARAM,-1.f,  1.f,    0.f,   "Scale Dur attenuverter");
        configParam(B_AMP_ATT_PARAM,    -1.f,  1.f,    0.f,   "B Amp attenuverter");
        configParam(B_DUR_ATT_PARAM,    -1.f,  1.f,    0.f,   "B Dur attenuverter");

        configInput(SCALE_AMP_INPUT, "Scale Amp CV");
        configInput(SCALE_DUR_INPUT, "Scale Dur CV");
        configInput(B_AMP_INPUT,     "Amplitude barrier CV");
        configInput(B_DUR_INPUT,     "Duration barrier CV");

        configOutput(AUDIO_OUTPUT,      "Audio");
        configOutput(CYCLE_TRIG_OUTPUT, "Cycle trigger");
        configOutput(FREQ_CV_OUTPUT,    "Frequency 1V/oct");
    }

    // ── DSP helpers ───────────────────────────────────────────────────────────

    // Reflect x into [lo, hi] by folding at each barrier until it lands inside
    // (Serra eq. 3). Guards against a degenerate zero-width range.
    static float reflect(float x, float lo, float hi) {
        if (hi <= lo) return lo;
        while (x < lo || x > hi) {
            if (x < lo) x = 2.f * lo - x;
            if (x > hi) x = 2.f * hi - x;
        }
        return x;
    }

    // Inverse-CDF samplers. `scale` is the distribution's spread parameter.
    static float cauchySample(float scale) {
        float u = clamp(rack::random::uniform(), 0.0001f, 0.9999f);
        return scale * std::tan(M_PI * (u - 0.5f));
    }

    static float gaussianSample(float scale) {
        // Box-Muller; clamp u1 away from 0 to avoid log(0).
        float u1 = clamp(rack::random::uniform(), 1e-6f, 1.f);
        float u2 = rack::random::uniform();
        return scale * std::sqrt(-2.f * std::log(u1)) * std::cos(2.f * M_PI * u2);
    }

    static float uniformSample(float scale) {
        return scale * (2.f * rack::random::uniform() - 1.f);
    }

    static float logisticSample(float scale) {
        float u = clamp(rack::random::uniform(), 0.0001f, 0.9999f);
        return scale * std::log(u / (1.f - u));
    }

    float drawSample(int dist, float scale) {
        switch (dist) {
            case 1:  return gaussianSample(scale);
            case 2:  return uniformSample(scale);
            case 3:  return logisticSample(scale);
            default: return cauchySample(scale);
        }
    }

    // ── Oscillator helpers ────────────────────────────────────────────────────

    // Randomise all breakpoints across the full barrier range and reset the
    // oscillator to breakpoint 0. Called on N change and at startup.
    void initBreakpoints(int N, float bAmp, float bDurMin, float bDurMax) {
        for (int i = 0; i < N; i++) {
            amp[i] = (2.f * rack::random::uniform() - 1.f) * bAmp;
            dur[i] = bDurMin + rack::random::uniform() * (bDurMax - bDurMin);
        }
        current_breakpoint = 0;
        current_sample     = 0;
        current_amp        = 0.f;
        target_amp         = amp[0];
        current_dur        = std::max(1, (int)dur[0]);
    }

    // Apply one cycle of the single-level random walk to all breakpoints
    // (Serra 1993, eq. 2). Each step is drawn from the chosen distribution,
    // mirrored into [-limit, +limit], added to the current value, then the
    // result is mirrored back into the barrier range.
    // Note: amp[0] is overwritten immediately after this call in process() to
    // satisfy the continuity condition; its value here is discarded.
    void runCycleUpdate(int N, float scaleAmp, float scaleDur,
                        float bAmp, float bDurMin, float bDurMax, int dist) {
        const float pDurHW   = (bDurMax - bDurMin) * 0.5f;
        const float limitAmp = scaleAmp * bAmp;
        const float limitDur = scaleDur * pDurHW;
        for (int i = 0; i < N; i++) {
            float sAmp = reflect(drawSample(dist, limitAmp), -limitAmp, limitAmp);
            amp[i]     = reflect(amp[i] + sAmp, -bAmp, bAmp);

            float sDur = reflect(drawSample(dist, limitDur), -limitDur, limitDur);
            dur[i]     = reflect(dur[i] + sDur, bDurMin, bDurMax);
        }
    }

    // ── Main DSP loop ─────────────────────────────────────────────────────────

    void process(const ProcessArgs& args) override {

        // ── Read parameters ───────────────────────────────────────────────────
        int N = clamp((int)params[N_PARAM].getValue(), 2, MAX_N);

        float centerFreq = clamp(params[B_DUR_CENTER_PARAM].getValue(), 20.f, 5000.f);

        float scaleAmp = params[SCALE_AMP_PARAM].getValue();
        if (inputs[SCALE_AMP_INPUT].isConnected())
            scaleAmp += inputs[SCALE_AMP_INPUT].getVoltage()
                      * params[SCALE_AMP_ATT_PARAM].getValue() * 0.1f;
        scaleAmp = clamp(scaleAmp, 0.f, 1.f);

        float scaleDur = params[SCALE_DUR_PARAM].getValue();
        if (inputs[SCALE_DUR_INPUT].isConnected())
            scaleDur += inputs[SCALE_DUR_INPUT].getVoltage()
                      * params[SCALE_DUR_ATT_PARAM].getValue() * 0.1f;
        scaleDur = clamp(scaleDur, 0.f, 1.f);

        float bAmp = params[B_AMP_PARAM].getValue();
        if (inputs[B_AMP_INPUT].isConnected())
            bAmp += inputs[B_AMP_INPUT].getVoltage()
                  * params[B_AMP_ATT_PARAM].getValue() * 0.1f;
        bAmp = clamp(bAmp, 0.f, 1.f);

        float bDurWidth = params[B_DUR_WIDTH_PARAM].getValue();
        if (inputs[B_DUR_INPUT].isConnected())
            bDurWidth += inputs[B_DUR_INPUT].getVoltage()
                       * params[B_DUR_ATT_PARAM].getValue() * 0.1f;
        bDurWidth = clamp(bDurWidth, 0.f, 1.f);

        // Average samples-per-breakpoint at the target frequency, ±bDurWidth
        // fraction of that centre value. bDurWidth=0 collapses to a 1-sample
        // window, giving a fixed pitch.
        const float durCenter = args.sampleRate / (centerFreq * (float)N);
        const float halfWidth = bDurWidth * durCenter;
        float bDurMin = std::max(1.f, durCenter - halfWidth);
        float bDurMax = durCenter + halfWidth;
        if (bDurMax <= bDurMin) bDurMax = bDurMin + 1.f;

        int dist = clamp((int)params[DISTRIBUTION_PARAM].getValue(), 0, 3);

        // ── Reinit on N change ────────────────────────────────────────────────
        if (N != last_N) {
            initBreakpoints(N, bAmp, bDurMin, bDurMax);
            last_N = N;
        }

        // ── Generate output sample ────────────────────────────────────────────
        const float t      = (float)current_sample / (float)current_dur;
        const float output = current_amp + t * (target_amp - current_amp);
        outputs[AUDIO_OUTPUT].setVoltage(output * 5.f);

        // ── Auxiliary outputs ─────────────────────────────────────────────────
        outputs[CYCLE_TRIG_OUTPUT].setVoltage(
            cycleTrigger.process(args.sampleTime) ? 10.f : 0.f);

        // Instantaneous frequency based on current duration values (1V/oct, C4=0V).
        float sumDur = 0.f;
        for (int i = 0; i < N; i++) sumDur += dur[i];
        if (sumDur > 0.f) {
            const float freq = args.sampleRate / sumDur;
            outputs[FREQ_CV_OUTPUT].setVoltage(
                clamp(std::log2(freq / dsp::FREQ_C4), -5.f, 5.f));
        }

        // ── Advance oscillator state ──────────────────────────────────────────
        if (++current_sample >= current_dur) {
            current_amp        = target_amp;
            current_sample     = 0;
            current_breakpoint = (current_breakpoint + 1) % N;

            if (current_breakpoint == 0) {
                runCycleUpdate(N, scaleAmp, scaleDur, bAmp, bDurMin, bDurMax, dist);
                // Continuity condition (Serra eq. 1): y_{0,j+1} = y_{N-1,j}.
                // The first breakpoint of each new waveform cycle is pinned to
                // the value we just finished, so the output is seamless at
                // every cycle boundary. The stochastic walk evolves breakpoints
                // 1..N-1; amp[0] is set here after runCycleUpdate overwrites it.
                amp[0] = current_amp;
                cycleTrigger.trigger(1e-3f);
            }

            target_amp  = amp[current_breakpoint];
            current_dur = std::max(1, (int)dur[current_breakpoint]);
        }
    }
};


// ─── Widget ───────────────────────────────────────────────────────────────────

struct GENDYWidget : ModuleWidget {

    void draw(const DrawArgs& args) override {
        ModuleWidget::draw(args);

        std::shared_ptr<Font> font = APP->window->loadFont(
            asset::system("res/fonts/DejaVuSans.ttf"));
        if (!font) return;

        nvgSave(args.vg);
        nvgFontFaceId(args.vg, font->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        const NVGcolor title  = nvgRGB(0xe0, 0xe0, 0xff);
        const NVGcolor bright = nvgRGB(0xaa, 0xaa, 0xcc);
        const NVGcolor dim    = nvgRGB(0x77, 0x77, 0x99);
        const NVGcolor lo     = nvgRGB(0x55, 0x55, 0x77);
        const NVGcolor outclr = nvgRGB(0xcc, 0xcc, 0xee);

        auto lbl = [&](float x, float y, float sz, NVGcolor col, const char* s) {
            nvgFontSize(args.vg, mm2px(sz));
            nvgFillColor(args.vg, col);
            nvgText(args.vg, mm2px(x), mm2px(y), s, nullptr);
        };

        auto hline = [&](float y) {
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, mm2px(2.5f), mm2px(y));
            nvgLineTo(args.vg, mm2px(38.f),  mm2px(y));
            nvgStrokeColor(args.vg, nvgRGB(0x33, 0x33, 0x55));
            nvgStrokeWidth(args.vg, 0.6f);
            nvgStroke(args.vg);
        };

        // Title
        nvgFontSize(args.vg, mm2px(4.2f));
        nvgTextLetterSpacing(args.vg, mm2px(0.3f));
        nvgFillColor(args.vg, title);
        nvgText(args.vg, mm2px(20.32f), mm2px(7.5f), "GENDYN", nullptr);
        nvgTextLetterSpacing(args.vg, 0.f);

        hline(13.f);
        hline(99.f);

        // Column headers for attenuverter / CV jack columns
        lbl(27.f, 15.5f, 1.7f, lo, "att");
        lbl(35.f, 15.5f, 1.7f, lo, "cv");

        // Parameter labels sit 5 mm below each knob centre, matching the
        // convention used by the output labels below their jacks.
        // Knob rows: 18, 30, 42, 54, 66, 78, 90 mm  →  labels at +5 mm.
        lbl(20.32f, 23.f, 1.9f, dim,    "N");
        lbl(14.f,   35.f, 1.9f, dim,    "SCALE AMP");
        lbl(14.f,   47.f, 1.9f, dim,    "SCALE DUR");
        lbl(14.f,   59.f, 1.9f, bright, "B AMP");
        lbl(14.f,   71.f, 1.9f, dim,    "B DUR CTR");
        lbl(14.f,   83.f, 1.9f, bright, "B DUR WID");
        lbl(20.32f, 95.f, 1.9f, dim,    "DIST");

        // Output labels
        lbl(7.f,    117.5f, 2.0f, outclr, "OUT");
        lbl(20.32f, 117.5f, 2.0f, outclr, "TRIG");
        lbl(33.f,   117.5f, 2.0f, outclr, "FREQ");

        nvgRestore(args.vg);
    }

    GENDYWidget(GENDYN* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/GENDYN.svg")));

        addChild(createWidget<ScrewSilver>(mm2px(Vec(1.0f,    1.0f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(34.62f,  1.0f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(1.0f,  122.0f))));
        addChild(createWidget<ScrewSilver>(mm2px(Vec(34.62f,122.0f))));

        addParam(createParamCentered<RoundBlackSnapKnob>(
            mm2px(Vec(20.32f, 18.f)), module, GENDYN::N_PARAM));

        // SCALE_AMP row: knob | att | cv
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 9.f, 30.f)), module, GENDYN::SCALE_AMP_PARAM));
        addParam(createParamCentered<Trimpot>(        mm2px(Vec(27.f, 30.f)), module, GENDYN::SCALE_AMP_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(     mm2px(Vec(35.f, 30.f)), module, GENDYN::SCALE_AMP_INPUT));

        // SCALE_DUR row
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 9.f, 42.f)), module, GENDYN::SCALE_DUR_PARAM));
        addParam(createParamCentered<Trimpot>(        mm2px(Vec(27.f, 42.f)), module, GENDYN::SCALE_DUR_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(     mm2px(Vec(35.f, 42.f)), module, GENDYN::SCALE_DUR_INPUT));

        // B_AMP row
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 9.f, 54.f)), module, GENDYN::B_AMP_PARAM));
        addParam(createParamCentered<Trimpot>(        mm2px(Vec(27.f, 54.f)), module, GENDYN::B_AMP_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(     mm2px(Vec(35.f, 54.f)), module, GENDYN::B_AMP_INPUT));

        // B_DUR_CENTER — frequency knob, no CV modulation
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(9.f, 66.f)), module, GENDYN::B_DUR_CENTER_PARAM));

        // B_DUR_WIDTH row
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 9.f, 78.f)), module, GENDYN::B_DUR_WIDTH_PARAM));
        addParam(createParamCentered<Trimpot>(        mm2px(Vec(27.f, 78.f)), module, GENDYN::B_DUR_ATT_PARAM));
        addInput(createInputCentered<PJ301MPort>(     mm2px(Vec(35.f, 78.f)), module, GENDYN::B_DUR_INPUT));

        addParam(createParamCentered<RoundBlackSnapKnob>(
            mm2px(Vec(20.32f, 90.f)), module, GENDYN::DISTRIBUTION_PARAM));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec( 7.f,   110.f)), module, GENDYN::AUDIO_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 110.f)), module, GENDYN::CYCLE_TRIG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(33.f,   110.f)), module, GENDYN::FREQ_CV_OUTPUT));
    }
};

Model* modelGENDYN = createModel<GENDYN, GENDYWidget>("GENDYN");
