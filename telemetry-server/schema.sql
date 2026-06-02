-- CarRadio Telemetry — PostgreSQL schema
-- Applied via docker-entrypoint-initdb.d on first postgres start.

-- 5.1 Device registry
CREATE TABLE devices (
  device_id           UUID PRIMARY KEY,
  api_key_hash        TEXT NOT NULL,
  first_seen          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  last_seen           TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  hardware_revision   TEXT,
  telemetry_salt      TEXT NOT NULL DEFAULT '',
  current_firmware    TEXT,
  telemetry_enabled   BOOLEAN NOT NULL DEFAULT TRUE,
  notes               TEXT
);

-- 5.2 Sessions (one per boot/drive)
CREATE TABLE sessions (
  session_id          UUID PRIMARY KEY,
  device_id           UUID NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  started_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  ended_at            TIMESTAMPTZ,
  firmware_version    TEXT NOT NULL,
  reset_reason        TEXT,
  telemetry_enabled   BOOLEAN NOT NULL
);
CREATE INDEX ON sessions(device_id, started_at DESC);

-- 5.3 Event fact table (append-only)
CREATE TABLE events (
  event_id            BIGSERIAL PRIMARY KEY,
  session_id          UUID NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
  device_ts_ms        BIGINT NOT NULL,
  server_ts           TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  event_type          TEXT NOT NULL,
  payload             JSONB NOT NULL DEFAULT '{}'::jsonb
);
CREATE INDEX events_session_ts  ON events (session_id, device_ts_ms);
CREATE INDEX events_type_brin   ON events USING BRIN (event_type);
CREATE INDEX events_payload_gin ON events USING GIN (payload jsonb_path_ops);

-- 5.4 Command queue
CREATE TABLE device_commands (
  command_id          BIGSERIAL PRIMARY KEY,
  device_id           UUID NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  command_type        TEXT NOT NULL,
  payload             JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at          TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  delivered_at        TIMESTAMPTZ,
  completed_at        TIMESTAMPTZ,
  result              JSONB
);
CREATE INDEX ON device_commands (device_id) WHERE completed_at IS NULL;

-- 5.5 Uploaded full-log files
CREATE TABLE full_log_uploads (
  upload_id           BIGSERIAL PRIMARY KEY,
  device_id           UUID NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  session_id          UUID REFERENCES sessions(session_id) ON DELETE SET NULL,
  command_id          BIGINT REFERENCES device_commands(command_id),
  uploaded_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  size_bytes          INTEGER NOT NULL,
  storage_path        TEXT NOT NULL
);

-- 5.6 Human-reported observations
CREATE TABLE observations (
  observation_id      BIGSERIAL PRIMARY KEY,
  session_id          UUID REFERENCES sessions(session_id) ON DELETE SET NULL,
  device_id           UUID REFERENCES devices(device_id) ON DELETE SET NULL,
  car_make            TEXT,
  car_model           TEXT,
  speaker_model       TEXT,
  note                TEXT NOT NULL,
  reported_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
