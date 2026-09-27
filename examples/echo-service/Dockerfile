FROM python:3.14.5-alpine3.22
RUN addgroup -S app && adduser -S -G app -u 10001 app
WORKDIR /app
COPY --chown=app:app app.py ./app.py
USER 10001:10001
ENV PORT=8080 PYTHONUNBUFFERED=1
EXPOSE 8080
HEALTHCHECK --interval=5s --timeout=2s --start-period=2s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8080/health || exit 1
ENTRYPOINT ["python3", "/app/app.py"]
