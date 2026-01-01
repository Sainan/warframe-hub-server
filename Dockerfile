FROM ghcr.io/calamity-inc/soup:4ca93073fbbb548a0337126c79543ac54288be81

COPY main.cpp /app
WORKDIR /app
RUN clang main.cpp -DDOCKER -LSoup -lsoup -ISoup/soup -std=c++17 -lstdc++ -fno-rtti -O3

ENTRYPOINT ["./a.out"]
