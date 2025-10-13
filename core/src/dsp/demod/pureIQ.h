// core/src/dsp/demod/pureIQ.h
#pragma once
#include "../processor.h"
#include "../loop/agc.h"
#include "../correction/dc_blocker.h"
#include "../convert/mono_to_stereo.h"
#include <dsp/filter/fir.h>
#include "../taps/low_pass.h"
#include "../taps/band_pass.h"
#include "../multirate/rational_resampler.h"

namespace dsp::demod {

    template <class T>
    class pureIQ : public Processor<complex_t, T> {
        using base_type = Processor<complex_t, T>;

    public:
        pureIQ() = default;

        void init(std::string name, stream<complex_t>* input, double bandwidth, double samplerate, double out_samplerate) {
            _name = name;
            _bandwidth = bandwidth;
            _inRate = samplerate;
            _outRate = out_samplerate;
            _centerFreq = 0.0f; // Фильтр вокруг 0 Гц
            if (_inRate == _outRate) {
                _filterEnabled = false;
            }
            else {
                _filterEnabled = true;
            }
            if (_bandwidth <= 0.0 || _inRate <= 0.0 || _bandwidth >= _inRate) {
                _bandwidth = 12500.0; // защита от некорректных значений
            }

            fprintf(stderr, "[pureIQ] init: input=%p, bw=%.1f, inRate=%.1f, outRate=%.1f\n",
                    input, bandwidth, samplerate, out_samplerate);

            _taps = taps::bandPass<complex_t>(
                _centerFreq - _bandwidth / 2.0,
                _centerFreq + _bandwidth / 2.0,
                _bandwidth / 10.0,
                _inRate,
                true);

            _fir.init(nullptr, _taps);
            _resampler.init(nullptr, _inRate, _outRate);

            base_type::init(input);
            // base_type::out.resizeBuffer(8192);
            fprintf(stderr, "[pureIQ] base_type::init done\n");
        }

        void setBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            if (bandwidth == _bandwidth) return;

            _bandwidth = bandwidth;

            std::lock_guard<std::mutex> lck2(lpfMtx);
            taps::free(_taps);

            _taps = taps::bandPass<complex_t>(
                _centerFreq - _bandwidth / 2.0,
                _centerFreq + _bandwidth / 2.0,
                _bandwidth / 10.0,
                _inRate,
                true);

            _fir.setTaps(_taps);
        }

        int process(int count, complex_t* in, T* out) {
            if (_filterEnabled) {
                _fir.process(count, in, _fir.out.writeBuf);
                return _resampler.process(count, _fir.out.writeBuf, out);
            }
            else {
                // Без фильтра и без ресемплера
                std::memcpy(out, in, sizeof(complex_t) * count);
                return count;
            }
        }

        void start() override {
            base_type::start(); // Это важно!
        }

        void stop() override {
            base_type::stop();
        }

        void setInput(dsp::stream<complex_t>* input) {
            base_type::setInput(input);
        }

        int run() override {
            if (!base_type::out.writeBuf) {
                fprintf(stderr, "[pureIQ] out not ready (no buffer)\n");
                return 0;
            }
            int count = base_type::_in->read();
            if (count <= 0) return -1;

            // fprintf(stderr, "[pureIQ] run(): read %d samples\n", count);

            int written = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // fprintf(stderr, "[pureIQ] run(): wrote %d samples\n", written);

            base_type::_in->flush();

            if (written <= 0) return 0;

            if (!base_type::out.swap(written)) {
                fprintf(stderr, "[pureIQ] WARNING: swap failed\n");
                return -1;
            }

            return written;
        }

        // dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }
        /// dsp::stream<complex_t>* getIQOutput() { return &this->out; }

    private:
        std::string _name;

        float _bandwidth = 12500.0;
        float _inRate = 2500000.0;
        float _outRate = 96000.0;
        float _centerFreq = 0.0;
        bool _filterEnabled = true;

        tap<complex_t> _taps;
        filter::FIR<complex_t, complex_t> _fir;
        multirate::RationalResampler<complex_t> _resampler;
        std::mutex lpfMtx;
    };

} // namespace dsp::demod