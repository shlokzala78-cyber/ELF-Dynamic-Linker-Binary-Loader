FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends build-essential binutils python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /project
COPY . .
RUN make all build/unit-tests && useradd --create-home --uid 10001 learner && chown -R learner:learner /project
USER learner
CMD ["make", "demo"]
