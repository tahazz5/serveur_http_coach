FROM gcc:15 AS build
WORKDIR /src
COPY src ./src
RUN g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -static src/main.cpp -o /atlas-server
FROM debian:bookworm-slim
COPY --from=build /atlas-server /usr/local/bin/atlas-server
USER 65534:65534
ENV HOST=0.0.0.0 PORT=8080 WORKERS=4
EXPOSE 8080
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/atlas-server"]
