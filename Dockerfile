FROM ghcr.io/calamity-inc/soup:a702e6001683c3f8e0f257a1247843dac543995d

COPY main.cpp /app
WORKDIR /app
RUN clang main.cpp -DDOCKER -LSoup -lsoup -ISoup/soup -std=c++17 -lstdc++ -fno-rtti -O3

ENTRYPOINT ["./a.out"]
