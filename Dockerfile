FROM alpine:latest

RUN apk add g++ cmake make libc-dev jsoncpp jsoncpp-dev
RUN apk add --no-cache nlohmann-json

WORKDIR /app

COPY . .

RUN cmake . && make -j4

CMD ["./kvstore"]
