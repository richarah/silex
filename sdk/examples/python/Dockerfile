FROM ghcr.io/richarah/silex:slim

# Install uv for fast Python dependency management
RUN pip install --no-cache-dir uv

WORKDIR /workspace
COPY pyproject.toml uv.lock ./

# Install dependencies (pip cache mounted for faster rebuilds)
RUN --mount=type=cache,target=/root/.cache/pip \
    uv pip install --system -r pyproject.toml

COPY . .

CMD ["python3", "src/main.py"]
