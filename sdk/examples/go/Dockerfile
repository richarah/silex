FROM ghcr.io/richarah/silex:slim

# Install Go
RUN apk add --no-cache go

WORKDIR /workspace
COPY . .

# Go module cache is mounted for faster repeated builds.
RUN --mount=type=cache,target=/root/go/pkg/mod \
    go build -o /usr/local/bin/my-app ./cmd/my-app

CMD ["my-app"]
