#include "wav.h"
#include <volk/volk.h>
#include <stdexcept>
#include <dsp/buffer/buffer.h>
#include <dsp/stream.h>
#include <map>
#include <algorithm> // НОВОЕ: для std::min

namespace wav
{
    const char *WAVE_FILE_TYPE = "WAVE";
    const char *FORMAT_MARKER = "fmt ";
    const char *DATA_MARKER = "data";
    const uint32_t FORMAT_HEADER_LEN = 16;
    const uint16_t SAMPLE_TYPE_PCM = 1;

    std::map<SampleType, int> SAMP_BITS = {
        {SAMP_TYPE_UINT8, 8},
        {SAMP_TYPE_INT16, 16},
        {SAMP_TYPE_INT32, 32},
        {SAMP_TYPE_FLOAT32, 32}};

    Writer::Writer(int channels, uint64_t samplerate, Format format, SampleType type)
    {
        // Validate channels and samplerate
        if (channels < 1)
        {
            throw std::runtime_error("Channel count must be greater or equal to 1");
        }
        if (!samplerate)
        {
            throw std::runtime_error("Samplerate must be non-zero");
        }

        // Initialize variables
        _channels = channels;
        _samplerate = samplerate;
        _format = format;
        _type = type;
    }

    Writer::~Writer() { close(); }

    bool Writer::open(std::string path)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Close previous file
        if (rw.isOpen())
        {
            close();
        }

        // Reset work values
        samplesWritten = 0;
        _bufferFill = 0; // НОВОЕ: Сбрасываем счетчик заполнения буфера

        // Fill header
        bytesPerSamp = (SAMP_BITS[_type] / 8) * _channels;
        hdr.codec = (_type == SAMP_TYPE_FLOAT32) ? CODEC_FLOAT : CODEC_PCM;
        hdr.channelCount = _channels;
        hdr.sampleRate = _samplerate;
        hdr.bitDepth = SAMP_BITS[_type];
        hdr.bytesPerSample = bytesPerSamp;
        hdr.bytesPerSecond = bytesPerSamp * _samplerate;

        // Precompute sizes and allocate buffers
        switch (_type)
        {
        case SAMP_TYPE_UINT8:
            bufU8 = dsp::buffer::alloc<uint8_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_INT16:
            bufI16 = dsp::buffer::alloc<int16_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_INT32:
            bufI32 = dsp::buffer::alloc<int32_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_FLOAT32:
            break;
        default:
            return false;
            break;
        }

        // НОВОЕ: Выделяем основной буфер для записи
        try
        {
            _internalBuffer = std::make_unique<uint8_t[]>(INTERNAL_BUFFER_SIZE);
        }
        catch (const std::bad_alloc &e)
        {
            // Не удалось выделить память, очищаем и выходим
            close();
            return false;
        }

        // Open file
        if (!rw.open(path, WAVE_FILE_TYPE))
        {
            return false;
        }

        // Write format chunk
        rw.beginChunk(FORMAT_MARKER);
        rw.write((uint8_t *)&hdr, sizeof(FormatHeader));
        rw.endChunk();

        // Begin data chunk
        rw.beginChunk(DATA_MARKER);

        return true;
    }

    bool Writer::isOpen()
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        return rw.isOpen();
    }

    void Writer::close()
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do nothing if the file is not open
        if (!rw.isOpen())
        {
            return;
        }

        // НОВОЕ: Сбрасываем на диск остаток данных из буфера ПЕРЕД закрытием чанка
        if (_bufferFill > 0)
        {
            rw.write(_internalBuffer.get(), _bufferFill);
            _bufferFill = 0;
        }

        // Finish data chunk
        rw.endChunk();

        // Close the file
        rw.close();

        // НОВОЕ: Освобождаем основной буфер
        _internalBuffer.reset();

        // Free buffers
        if (bufU8)
        {
            dsp::buffer::free(bufU8);
            bufU8 = NULL;
        }
        if (bufI16)
        {
            dsp::buffer::free(bufI16);
            bufI16 = NULL;
        }
        if (bufI32)
        {
            dsp::buffer::free(bufI32);
            bufI32 = NULL;
        }
    }

    void Writer::setChannels(int channels)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (rw.isOpen())
        {
            throw std::runtime_error("Cannot change parameters while file is open");
        }

        // Validate channel count
        if (channels < 1)
        {
            throw std::runtime_error("Channel count must be greater or equal to 1");
        }
        _channels = channels;
    }

    void Writer::setSamplerate(uint64_t samplerate)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (rw.isOpen())
        {
            throw std::runtime_error("Cannot change parameters while file is open");
        }

        // Validate samplerate
        if (!samplerate)
        {
            throw std::runtime_error("Samplerate must be non-zero");
        }
        _samplerate = samplerate;
    }

    void Writer::setFormat(Format format)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (rw.isOpen())
        {
            throw std::runtime_error("Cannot change parameters while file is open");
        }
        _format = format;
    }

    void Writer::setSampleType(SampleType type)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (rw.isOpen())
        {
            throw std::runtime_error("Cannot change parameters while file is open");
        }
        _type = type;
    }

    void Writer::write(float *samples, int count)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!rw.isOpen())
            return;

        const int maxFramesPerChunk = STREAM_BUFFER_SIZE;             // гарантированный размер промежуточных буферов
        const int maxSamplesPerChunk = maxFramesPerChunk * _channels; // для volk-конвертации

        int framesLeft = count;
        int frameOffset = 0;

        while (framesLeft > 0)
        {
            const int framesNow = std::min(framesLeft, maxFramesPerChunk);
            const int samplesNow = framesNow * _channels;
            const int bytesNow = framesNow * bytesPerSamp;

            const float *in = samples + (size_t)frameOffset * _channels;
            const uint8_t *dataToWrite = nullptr;

            switch (_type)
            {
            case SAMP_TYPE_UINT8:
                // volk не умеет u8 — заполняем bufU8 вручную, но строго в пределах выделенного буфера
                for (int i = 0; i < samplesNow; i++)
                    bufU8[i] = (in[i] * 127.0f) + 128.0f;
                dataToWrite = bufU8;
                break;

            case SAMP_TYPE_INT16:
                volk_32f_s32f_convert_16i(bufI16, in, 32767.0f, samplesNow);
                dataToWrite = reinterpret_cast<const uint8_t *>(bufI16);
                break;

            case SAMP_TYPE_INT32:
                volk_32f_s32f_convert_32i(bufI32, in, 2147483647.0f, samplesNow);
                dataToWrite = reinterpret_cast<const uint8_t *>(bufI32);
                break;

            case SAMP_TYPE_FLOAT32:
                dataToWrite = reinterpret_cast<const uint8_t *>(in);
                break;

            default:
                return;
            }

            if (dataToWrite)
                _write_buffered(dataToWrite, (size_t)bytesNow);

            frameOffset += framesNow;
            framesLeft -= framesNow;
        }

        // учёт написанных сэмплов — по кадрам (frames)
        samplesWritten += count;
    }
    /*
    void Writer::write(float *samples, int count)
    {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!rw.isOpen())
        {
            return;
        }

        // Select different writer function depending on the chose depth
        int tcount = count * _channels;
        int tbytes = count * bytesPerSamp;
        const uint8_t *dataToWrite = nullptr;

        switch (_type)
        {
        case SAMP_TYPE_UINT8:
            for (int i = 0; i < tcount; i++)
            {
                bufU8[i] = (samples[i] * 127.0f) + 128.0f;
            }
            dataToWrite = bufU8;
            break;
        case SAMP_TYPE_INT16:
            volk_32f_s32f_convert_16i(bufI16, samples, 32767.0f, tcount);
            dataToWrite = (uint8_t *)bufI16;
            break;
        case SAMP_TYPE_INT32:
            volk_32f_s32f_convert_32i(bufI32, samples, 2147483647.0f, tcount);
            dataToWrite = (uint8_t *)bufI32;
            break;
        case SAMP_TYPE_FLOAT32:
            dataToWrite = (uint8_t *)samples;
            break;
        default:
            return;
        }

        // Вызываем новый метод для буферизованной записи
        if (dataToWrite)
        {
            _write_buffered(dataToWrite, tbytes);
        }

        // Increment sample counter
        samplesWritten += count;
    }
    */
    // НОВОЕ: Реализация буферизованной записи
    void Writer::_write_buffered(const uint8_t *data, size_t bytes)
    {
        size_t bytesCopied = 0;
        while (bytesCopied < bytes)
        {
            // Сколько места осталось в буфере?
            size_t spaceLeft = INTERNAL_BUFFER_SIZE - _bufferFill;
            // Сколько скопировать на этой итерации? (минимум из того, что нужно, и того, что влезет)
            size_t toCopy = std::min(bytes - bytesCopied, spaceLeft);

            // Копируем данные в наш внутренний буфер
            memcpy(_internalBuffer.get() + _bufferFill, data + bytesCopied, toCopy);
            _bufferFill += toCopy;
            bytesCopied += toCopy;

            // Если буфер заполнился - сбрасываем его на диск
            if (_bufferFill == INTERNAL_BUFFER_SIZE)
            {
                rw.write(_internalBuffer.get(), INTERNAL_BUFFER_SIZE);
                _bufferFill = 0; // и обнуляем счетчик
            }
        }
    }

}
