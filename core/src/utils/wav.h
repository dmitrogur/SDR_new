#pragma once
#include <string>
#include <fstream>
#include <stdint.h>
#include <mutex>
#include <memory> // НОВОЕ: для std::unique_ptr
#include "riff.h"

namespace wav {    
    #pragma pack(push, 1)
    struct FormatHeader {
        uint16_t codec;
        uint16_t channelCount;
        uint32_t sampleRate;
        uint32_t bytesPerSecond;
        uint16_t bytesPerSample;
        uint16_t bitDepth;
    };
    #pragma pack(pop)

    enum Format {
        FORMAT_WAV,
        FORMAT_RF64
    };

    enum SampleType {
        SAMP_TYPE_UINT8,
        SAMP_TYPE_INT16,
        SAMP_TYPE_INT32,
        SAMP_TYPE_FLOAT32
    };

    enum Codec {
        CODEC_PCM   = 1,
        CODEC_FLOAT = 3
    };

    class Writer {
    public:
        Writer(int channels = 2, uint64_t samplerate = 16000, Format format = FORMAT_WAV, SampleType type = SAMP_TYPE_INT16);
        ~Writer();

        bool open(std::string path);
        bool isOpen();
        void close();

        void setChannels(int channels);
        void setSamplerate(uint64_t samplerate);
        void setFormat(Format format);
        void setSampleType(SampleType type);

        size_t getSamplesWritten() { return samplesWritten; }

        void write(float* samples, int count);

    private:
        // НОВОЕ: Приватный метод для записи данных через буфер
        void _write_buffered(const uint8_t* data, size_t bytes);

        std::recursive_mutex mtx;
        FormatHeader hdr;
        riff::Writer rw;

        int _channels;
        uint64_t _samplerate;
        Format _format;
        SampleType _type;
        size_t bytesPerSamp;

        uint8_t* bufU8 = NULL;
        int16_t* bufI16 = NULL;
        int32_t* bufI32 = NULL;
        size_t samplesWritten = 0;

        // --- НОВЫЕ ПОЛЯ ДЛЯ БУФЕРИЗАЦИИ ---
        static const size_t INTERNAL_BUFFER_SIZE = 2 * 1024 * 1024; // 2 MB буфер
        std::unique_ptr<uint8_t[]> _internalBuffer;
        size_t _bufferFill = 0;        
    };
}
