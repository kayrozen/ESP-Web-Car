FROM golang:1.22-alpine AS builder

RUN apk add --no-cache git ca-certificates

WORKDIR /app
COPY go.mod ./
RUN go mod download || true
COPY . .
RUN go mod tidy && \
    CGO_ENABLED=0 GOOS=linux go build -ldflags="-s -w" -o /tm-api .

# ── Runtime image ──────────────────────────────────────────────────────
FROM scratch
COPY --from=builder /tm-api /tm-api
COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/

EXPOSE 8080
ENTRYPOINT ["/tm-api"]
