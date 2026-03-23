# Contributing to Silex

**Note**: This project is currently at v0.1. Full contribution guidelines will be established for v0.2 when the project is publicly available on GitHub.

## Development Status

- **v0.1** (current): All core features implemented, 43 tests passing — accepting bug reports
- **v0.2**: Public beta with CI/CD — accepting contributions
- **v1.0**: Public launch, full contribution workflow

## Getting Started

If you want to experiment with Silex:

```bash
# Clone the repository
git clone https://github.com/richarah/silex.git
cd silex

# Set up git hooks (one-time, required for contributors)
./scripts/setup-dev.sh

# Build the image
./scripts/build.sh

# Run benchmarks
cd benchmarks
./benchmark.sh

# Test in your own Dockerfile
docker build -f your-dockerfile --build-arg BASE_IMAGE=silex:slim .
```

## Reporting Issues

For v0.2+, open issues on GitHub for:
- Build failures
- Performance regressions
- Documentation errors
- Package mapping mistakes (apt-shim)
- Feature requests

## Code Style

- Shell scripts: Follow Google Shell Style Guide
- Dockerfiles: Comments above every non-obvious RUN command
- Documentation: British English, technical tone, no marketing copy

## Testing

Before submitting changes:
1. Build all images: `./scripts/build.sh`
2. Run tests: `./scripts/test.sh` (v0.1+)
3. Run benchmarks: `cd benchmarks && ./benchmark.sh`
4. Verify image size hasn't increased unexpectedly

## Commit Messages

Use conventional commits:
- `feat: add GPU support to Dockerfile.full`
- `fix: correct package mapping for libssl-dev`
- `docs: update README with new examples`
- `perf: reduce image size by 50MB`

## Pull Request Process

(For v0.2+)

1. Fork the repository
2. Create a feature branch: `git checkout -b feat/your-feature`
3. Make your changes
4. Run tests and benchmarks
5. Commit with conventional commit messages
6. Push and open a PR
7. Wait for CI to pass and maintainer review

## Areas for Contribution

Future areas where contributions will be welcome:

- **Package mappings**: Expanding `config/package-mapping.json` with more Debian packages
- **Example Dockerfiles**: Real-world examples for different languages and frameworks
- **Documentation**: Migration guides, troubleshooting, best practices
- **Testing**: Test scripts for different build scenarios
- **Benchmarking**: Additional benchmark projects, different languages
- **Tooling**: Improvements to `silex doctor`, `silex lint`, etc.

## Questions?

For now, open an issue on GitHub (v0.2+) or email (details in README once public).

---

**Thank you for your interest in Silex!**
