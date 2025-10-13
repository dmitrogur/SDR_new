build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_AUDIO_SINK=ON

all: build
	cmake --build build -j4