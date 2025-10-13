#pragma once
#include "../sink.h"
#include <vector>    // Добавим для std::vector
#include <algorithm> // Добавим для std::find
#include <stdexcept> // Добавим для std::runtime_error

namespace dsp::routing
{
    template <class T>
    class Splitter : public Sink<T>
    {
        using base_type = Sink<T>;

    public:
        Splitter() {}

        Splitter(stream<T> *in) { base_type::init(in); }

        void bindStream(stream<T> *stream)
        {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            assert(base_type::_block_init);

            // Check that the stream isn't already bound
            if (std::find(streams.begin(), streams.end(), stream) != streams.end())
            {
                // Уберем исключение и заменим на предупреждение для большей стабильности
                // throw std::runtime_error("[Splitter] Tried to bind stream to that is already bound");
                return;
            }

            // Add to the list
            base_type::tempStop();
            base_type::registerOutput(stream);
            streams.push_back(stream);
            base_type::tempStart();
        }

        void unbindStream(stream<T> *stream)
        {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            assert(base_type::_block_init);

            // Check that the stream is bound
            auto sit = std::find(streams.begin(), streams.end(), stream);
            if (sit == streams.end())
            {
                // Уберем исключение и заменим на предупреждение для большей стабильности
                // throw std::runtime_error("[Splitter] Tried to unbind stream to that isn't bound");
                return;
            }

            // Add to the list
            base_type::tempStop();
            streams.erase(sit);
            base_type::unregisterOutput(stream);
            base_type::tempStart();
        }

        // ====== ВОТ НОВЫЙ МЕТОД В ПРАВИЛЬНОМ МЕСТЕ ======
        bool isStreamBound(dsp::stream<T> *stream)
        {
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            for (const auto &bound_stream : this->streams)
            {
                if (bound_stream == stream)
                {
                    return true;
                }
            }
            return false;
        }

        /*
        void bindStream(stream<T> *stream)
        {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            // Check that the stream isn't already bound
            if (std::find(streams.begin(), streams.end(), stream) != streams.end())
            {
                throw std::runtime_error("[Splitter] Tried to bind stream to that is already bound");
            }

            // Add to the list
            base_type::tempStop();
            base_type::registerOutput(stream);
            streams.push_back(stream);
            base_type::tempStart();
        }

        void unbindStream(stream<T> *stream)
        {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            // Check that the stream is bound
            auto sit = std::find(streams.begin(), streams.end(), stream);
            if (sit == streams.end())
            {
                throw std::runtime_error("[Splitter] Tried to unbind stream to that isn't bound");
            }

            // Add to the list
            base_type::tempStop();
            streams.erase(sit);
            base_type::unregisterOutput(stream);
            base_type::tempStart();
        }
        */
        int run()
        {
            int count = base_type::_in->read();
            if (count < 0)
            {
                return -1;
            }
            {
                std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
                for (const auto &stream : streams)
                {
                    memcpy(stream->writeBuf, base_type::_in->readBuf, count * sizeof(T));
                    if (!stream->swap(count))
                    {
                        base_type::_in->flush();
                        return -1;
                    }
                }
            }

            base_type::_in->flush();

            return count;
        }

    protected:
        std::vector<stream<T> *> streams;

    private:
        std::recursive_mutex ctrlMtx;
    };
}