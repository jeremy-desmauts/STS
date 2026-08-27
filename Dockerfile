FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go/go.mod ./
COPY go/main.go ./
RUN CGO_ENABLED=0 go build -o /sts-test .

FROM gcr.io/distroless/static-debian12
COPY --from=build /sts-test /sts-test
ENTRYPOINT ["/sts-test"]
