FROM alpine:latest

RUN apk add g++ cmake make libc-dev

WORKDIR /app

COPY . .

RUN cmake . && make -j4

CMD ["./kvstore"]
