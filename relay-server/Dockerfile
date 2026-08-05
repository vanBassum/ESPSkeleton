FROM python:3.13-slim

# Unbuffered so `docker logs` shows a line when it is written rather than when
# the pipe buffer fills; no bytecode in an image that gets rebuilt, not restarted.
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /app

# requirements.txt first: it changes far less often than relay.py, so the pip
# layer stays cached across ordinary code pushes.
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY relay.py ./

# Non-root. The relay binds 8080 (unprivileged) and writes nothing outside
# /app/data, which is where the SQLite state lands once step 5 exists.
RUN useradd --system --create-home --uid 10001 relay \
 && mkdir -p /app/data \
 && chown relay:relay /app/data
USER relay

EXPOSE 8080

# slim ships neither curl nor wget, so the check is stdlib Python against /healthz.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/healthz', timeout=4)"]

ENTRYPOINT ["python", "relay.py"]
# --db on the volume, so which devices are approved survives a restart or an
# image update.
CMD ["--host", "0.0.0.0", "--port", "8080", "--db", "/app/data/relay.db"]
