-- SDR Acquisition — PostgreSQL schema
-- Run once before starting sdr_acquisition:
--   psql -U sdr -d sdr_scanner -f schema/init.sql

CREATE TABLE IF NOT EXISTS detections (
    id              BIGSERIAL       PRIMARY KEY,
    detected_at     TIMESTAMPTZ     NOT NULL,
    center_freq_hz  BIGINT          NOT NULL,  -- Hz
    bandwidth_hz    INTEGER         NOT NULL,  -- Hz
    power_db        REAL            NOT NULL,  -- peak power above noise floor, dB
    scanner_id      TEXT            NOT NULL,  -- matches scanner_id in scanner.xml
    channel         SMALLINT        NOT NULL   -- RX channel index on the device
);

CREATE INDEX IF NOT EXISTS idx_detections_time ON detections (detected_at DESC);
CREATE INDEX IF NOT EXISTS idx_detections_freq ON detections (center_freq_hz);
CREATE INDEX IF NOT EXISTS idx_detections_scanner ON detections (scanner_id, detected_at DESC);

-- Useful views for querying ──────────────────────────────────────────────────

-- Signals seen in the last 60 seconds
CREATE OR REPLACE VIEW recent_detections AS
SELECT
    detected_at,
    round(center_freq_hz / 1e6, 3)  AS freq_mhz,
    round(bandwidth_hz   / 1e3, 1)  AS bw_khz,
    round(power_db::numeric, 1)     AS power_db,
    scanner_id,
    channel
FROM detections
WHERE detected_at > now() - interval '60 seconds'
ORDER BY detected_at DESC;

-- Per-frequency activity summary (useful for building a persistence map)
CREATE OR REPLACE VIEW freq_activity AS
SELECT
    round(center_freq_hz / 1e6, 2)  AS freq_mhz,
    count(*)                         AS hit_count,
    round(avg(power_db)::numeric, 1) AS avg_power_db,
    round(max(power_db)::numeric, 1) AS peak_power_db,
    min(detected_at)                 AS first_seen,
    max(detected_at)                 AS last_seen
FROM detections
GROUP BY round(center_freq_hz / 1e6, 2)
ORDER BY hit_count DESC;
