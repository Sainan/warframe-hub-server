FROM ghcr.io/calamity-inc/soup:0f364e902692d0acf65888a7f77338d91a4f77c1

COPY main.cpp /app
WORKDIR /app
RUN clang main.cpp -DDOCKER -LSoup -lsoup -ISoup/soup -std=c++17 -lstdc++ -fno-rtti

ENTRYPOINT ["./a.out"]
