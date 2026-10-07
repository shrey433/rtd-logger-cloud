FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    RTD_DB=/data/rtd.db

WORKDIR /srv
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app ./app

# SQLite lives here; mount a persistent volume on /data or readings vanish on redeploy.
VOLUME /data
EXPOSE 8000

CMD ["sh", "-c", "uvicorn app.main:app --host 0.0.0.0 --port ${PORT:-8000} --proxy-headers --forwarded-allow-ips='*'"]
