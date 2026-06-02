package main

import (
	"crypto/rand"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	_ "github.com/lib/pq"
	"golang.org/x/crypto/argon2"
)

// ── Types ─────────────────────────────────────────────────────────────

type handler struct {
	db         *sql.DB
	adminToken string
	uploadDir  string
}

// ── Argon2 helpers ────────────────────────────────────────────────────

const (
	argon2Time    = 3
	argon2Memory  = 64 * 1024
	argon2Threads = 4
	argon2KeyLen  = 32
)

func hashAPIKey(key string) (string, error) {
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	hash := argon2.IDKey([]byte(key), salt, argon2Time, argon2Memory, argon2Threads, argon2KeyLen)
	return hex.EncodeToString(salt) + ":" + hex.EncodeToString(hash), nil
}

func verifyAPIKey(key, stored string) bool {
	parts := strings.SplitN(stored, ":", 2)
	if len(parts) != 2 {
		return false
	}
	salt, err1 := hex.DecodeString(parts[0])
	expected, err2 := hex.DecodeString(parts[1])
	if err1 != nil || err2 != nil {
		return false
	}
	actual := argon2.IDKey([]byte(key), salt, argon2Time, argon2Memory, argon2Threads, argon2KeyLen)
	return subtle.ConstantTimeCompare(actual, expected) == 1
}

// ── Auth ──────────────────────────────────────────────────────────────

// parseBearer splits "Bearer <token>" from the Authorization header.
func parseBearer(r *http.Request) string {
	auth := r.Header.Get("Authorization")
	if !strings.HasPrefix(auth, "Bearer ") {
		return ""
	}
	return strings.TrimPrefix(auth, "Bearer ")
}

// authenticateDevice validates "Bearer <device_id>:<api_key>".
// Returns the device UUID on success or "" on failure.
func (h *handler) authenticateDevice(r *http.Request) string {
	token := parseBearer(r)
	if token == "" {
		return ""
	}
	idx := strings.Index(token, ":")
	if idx < 0 {
		return ""
	}
	deviceID := token[:idx]
	apiKey := token[idx+1:]
	if deviceID == "" || apiKey == "" {
		return ""
	}

	var storedHash string
	err := h.db.QueryRow(
		`SELECT api_key_hash FROM devices WHERE device_id = $1`, deviceID,
	).Scan(&storedHash)
	if err != nil {
		return ""
	}
	if !verifyAPIKey(apiKey, storedHash) {
		return ""
	}

	// Bump last_seen
	_, _ = h.db.Exec(`UPDATE devices SET last_seen = NOW() WHERE device_id = $1`, deviceID)
	return deviceID
}

func (h *handler) authenticateAdmin(r *http.Request) bool {
	return parseBearer(r) == h.adminToken
}

// ── JSON helpers ──────────────────────────────────────────────────────

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// ── Device handlers ───────────────────────────────────────────────────

// POST /api/v1/register
// Body: {"device_id":"...","api_key":"...","hardware_revision":"..."}
func (h *handler) register(w http.ResponseWriter, r *http.Request) {
	var req struct {
		DeviceID         string `json:"device_id"`
		APIKey           string `json:"api_key"`
		HardwareRevision string `json:"hardware_revision"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid json")
		return
	}
	if req.DeviceID == "" || req.APIKey == "" {
		writeErr(w, http.StatusBadRequest, "device_id and api_key required")
		return
	}

	keyHash, err := hashAPIKey(req.APIKey)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "hash error")
		return
	}

	_, err = h.db.Exec(`
		INSERT INTO devices (device_id, api_key_hash, hardware_revision)
		VALUES ($1, $2, $3)
		ON CONFLICT (device_id) DO UPDATE
		  SET api_key_hash = EXCLUDED.api_key_hash,
		      hardware_revision = COALESCE(EXCLUDED.hardware_revision, devices.hardware_revision),
		      last_seen = NOW()
	`, req.DeviceID, keyHash, req.HardwareRevision)
	if err != nil {
		log.Printf("register: db error: %v", err)
		writeErr(w, http.StatusInternalServerError, "db error")
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// POST /api/v1/sessions
// Body: {"session_id":"...","firmware_version":"...","reset_reason":"...","telemetry_enabled":true}
func (h *handler) createSession(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	var req struct {
		SessionID        string `json:"session_id"`
		FirmwareVersion  string `json:"firmware_version"`
		ResetReason      string `json:"reset_reason"`
		TelemetryEnabled bool   `json:"telemetry_enabled"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid json")
		return
	}
	if req.SessionID == "" {
		writeErr(w, http.StatusBadRequest, "session_id required")
		return
	}

	_, err := h.db.Exec(`
		INSERT INTO sessions (session_id, device_id, firmware_version, reset_reason, telemetry_enabled)
		VALUES ($1, $2, $3, $4, $5)
		ON CONFLICT (session_id) DO NOTHING
	`, req.SessionID, deviceID, req.FirmwareVersion, req.ResetReason, req.TelemetryEnabled)
	if err != nil {
		log.Printf("createSession: db error: %v", err)
		writeErr(w, http.StatusInternalServerError, "db error")
		return
	}

	// Update current_firmware on device
	_, _ = h.db.Exec(`UPDATE devices SET current_firmware = $1 WHERE device_id = $2`,
		req.FirmwareVersion, deviceID)

	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// POST /api/v1/events
// Body: [{"session_id":"...","device_ts_ms":N,"event_type":"...","payload":{...}}, ...]
func (h *handler) ingestEvents(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	var events []struct {
		SessionID   string          `json:"session_id"`
		DeviceTsMs  int64           `json:"device_ts_ms"`
		EventType   string          `json:"event_type"`
		Payload     json.RawMessage `json:"payload"`
	}
	if err := json.NewDecoder(r.Body).Decode(&events); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid json")
		return
	}
	if len(events) == 0 {
		writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "inserted": "0"})
		return
	}

	// Validate all session_ids belong to this device (check first occurrence of each)
	seenSessions := map[string]bool{}
	for _, e := range events {
		if !seenSessions[e.SessionID] {
			var ownerID string
			err := h.db.QueryRow(
				`SELECT device_id FROM sessions WHERE session_id = $1`, e.SessionID,
			).Scan(&ownerID)
			if err != nil || ownerID != deviceID {
				writeErr(w, http.StatusForbidden, "session not owned by device")
				return
			}
			seenSessions[e.SessionID] = true
		}
	}

	// Batch insert in a transaction
	tx, err := h.db.Begin()
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "tx error")
		return
	}
	stmt, err := tx.Prepare(`
		INSERT INTO events (session_id, device_ts_ms, event_type, payload)
		VALUES ($1, $2, $3, $4)
	`)
	if err != nil {
		_ = tx.Rollback()
		writeErr(w, http.StatusInternalServerError, "prepare error")
		return
	}
	defer stmt.Close()

	for _, e := range events {
		payload := e.Payload
		if len(payload) == 0 {
			payload = json.RawMessage(`{}`)
		}
		if _, err := stmt.Exec(e.SessionID, e.DeviceTsMs, e.EventType, string(payload)); err != nil {
			log.Printf("ingestEvents: insert error: %v", err)
		}
	}

	if err := tx.Commit(); err != nil {
		writeErr(w, http.StatusInternalServerError, "commit error")
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "inserted": len(events)})
}

// GET /api/v1/commands — returns at most 10 pending commands, marks delivered_at
func (h *handler) getCommands(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	rows, err := h.db.Query(`
		UPDATE device_commands
		SET delivered_at = NOW()
		WHERE command_id IN (
			SELECT command_id FROM device_commands
			WHERE device_id = $1
			  AND completed_at IS NULL
			  AND (delivered_at IS NULL OR delivered_at < NOW() - INTERVAL '5 minutes')
			ORDER BY created_at
			LIMIT 10
		)
		RETURNING command_id, command_type, payload
	`, deviceID)
	if err != nil {
		log.Printf("getCommands: %v", err)
		writeJSON(w, http.StatusOK, []any{})
		return
	}
	defer rows.Close()

	type command struct {
		CommandID   int64           `json:"command_id"`
		CommandType string          `json:"command_type"`
		Payload     json.RawMessage `json:"payload"`
	}
	commands := []command{}
	for rows.Next() {
		var c command
		var payloadStr string
		if err := rows.Scan(&c.CommandID, &c.CommandType, &payloadStr); err != nil {
			continue
		}
		c.Payload = json.RawMessage(payloadStr)
		commands = append(commands, c)
	}

	writeJSON(w, http.StatusOK, commands)
}

// POST /api/v1/commands/{id}/complete
// Body: {"success":true,"result":{...}}
func (h *handler) completeCommand(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	cmdIDStr := r.PathValue("id")
	cmdID, err := strconv.ParseInt(cmdIDStr, 10, 64)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "invalid command id")
		return
	}

	var req struct {
		Success bool            `json:"success"`
		Result  json.RawMessage `json:"result"`
	}
	_ = json.NewDecoder(r.Body).Decode(&req)

	result := string(req.Result)
	if result == "" || result == "null" {
		result = "{}"
	}

	res, err := h.db.Exec(`
		UPDATE device_commands
		SET completed_at = NOW(), result = $1
		WHERE command_id = $2 AND device_id = $3
	`, result, cmdID, deviceID)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "db error")
		return
	}
	if n, _ := res.RowsAffected(); n == 0 {
		writeErr(w, http.StatusNotFound, "command not found")
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// POST /api/v1/uploads — receive a full log dump (JSON body)
// Headers: X-Command-ID (optional)
func (h *handler) upload(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	cmdIDStr := r.Header.Get("X-Command-ID")
	var cmdID sql.NullInt64
	if n, err := strconv.ParseInt(cmdIDStr, 10, 64); err == nil {
		cmdID = sql.NullInt64{Int64: n, Valid: true}
	}

	// Read body (limit 20MB)
	body, err := io.ReadAll(io.LimitReader(r.Body, 20*1024*1024))
	if err != nil || len(body) == 0 {
		writeErr(w, http.StatusBadRequest, "empty body")
		return
	}

	// Save to disk
	ts := time.Now().UTC().Format("20060102T150405")
	filename := fmt.Sprintf("%s_%s.json", deviceID, ts)
	fpath := filepath.Join(h.uploadDir, filename)
	if err := os.WriteFile(fpath, body, 0644); err != nil {
		log.Printf("upload: write file: %v", err)
		writeErr(w, http.StatusInternalServerError, "storage error")
		return
	}

	_, err = h.db.Exec(`
		INSERT INTO full_log_uploads (device_id, command_id, size_bytes, storage_path)
		VALUES ($1, $2, $3, $4)
	`, deviceID, cmdID, len(body), filename)
	if err != nil {
		log.Printf("upload: db insert: %v", err)
	}

	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "bytes": len(body)})
}

// POST /api/v1/delete — GDPR self-delete
func (h *handler) deleteSelf(w http.ResponseWriter, r *http.Request) {
	deviceID := h.authenticateDevice(r)
	if deviceID == "" {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	h.deleteDevice(deviceID)
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}

// ── Admin handlers ────────────────────────────────────────────────────

// POST /api/admin/devices/{id}/commands
// Body: {"command_type":"upload_full_log","payload":{}}
func (h *handler) adminQueueCommand(w http.ResponseWriter, r *http.Request) {
	if !h.authenticateAdmin(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	deviceID := r.PathValue("id")
	var req struct {
		CommandType string          `json:"command_type"`
		Payload     json.RawMessage `json:"payload"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.CommandType == "" {
		writeErr(w, http.StatusBadRequest, "command_type required")
		return
	}

	payload := string(req.Payload)
	if payload == "" || payload == "null" {
		payload = "{}"
	}

	var cmdID int64
	err := h.db.QueryRow(`
		INSERT INTO device_commands (device_id, command_type, payload)
		VALUES ($1, $2, $3)
		RETURNING command_id
	`, deviceID, req.CommandType, payload).Scan(&cmdID)
	if err != nil {
		log.Printf("adminQueueCommand: %v", err)
		writeErr(w, http.StatusInternalServerError, "db error")
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"command_id": cmdID})
}

// POST /api/admin/devices/{id}/delete
func (h *handler) adminDeleteDevice(w http.ResponseWriter, r *http.Request) {
	if !h.authenticateAdmin(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	deviceID := r.PathValue("id")
	h.deleteDevice(deviceID)
	writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}

// GET /api/admin/devices/{id}/summary
func (h *handler) adminDeviceSummary(w http.ResponseWriter, r *http.Request) {
	if !h.authenticateAdmin(r) {
		writeErr(w, http.StatusUnauthorized, "unauthorized")
		return
	}

	deviceID := r.PathValue("id")

	var summary struct {
		DeviceID        string         `json:"device_id"`
		FirstSeen       time.Time      `json:"first_seen"`
		LastSeen        time.Time      `json:"last_seen"`
		Firmware        sql.NullString `json:"-"`
		FirmwareStr     string         `json:"current_firmware"`
		TotalSessions   int            `json:"total_sessions"`
		TotalEvents     int            `json:"total_events"`
		PendingCommands int            `json:"pending_commands"`
	}

	err := h.db.QueryRow(`
		SELECT d.device_id, d.first_seen, d.last_seen, d.current_firmware,
		       COUNT(DISTINCT s.session_id) AS total_sessions,
		       COUNT(e.event_id)            AS total_events,
		       (SELECT COUNT(*) FROM device_commands dc
		        WHERE dc.device_id = d.device_id AND dc.completed_at IS NULL) AS pending_commands
		FROM devices d
		LEFT JOIN sessions s ON s.device_id = d.device_id
		LEFT JOIN events   e ON e.session_id = s.session_id
		WHERE d.device_id = $1
		GROUP BY d.device_id, d.first_seen, d.last_seen, d.current_firmware
	`, deviceID).Scan(
		&summary.DeviceID, &summary.FirstSeen, &summary.LastSeen,
		&summary.Firmware, &summary.TotalSessions,
		&summary.TotalEvents, &summary.PendingCommands,
	)
	if err == sql.ErrNoRows {
		writeErr(w, http.StatusNotFound, "device not found")
		return
	}
	if err != nil {
		writeErr(w, http.StatusInternalServerError, "db error")
		return
	}

	summary.FirmwareStr = summary.Firmware.String
	writeJSON(w, http.StatusOK, summary)
}

// ── Delete helper ─────────────────────────────────────────────────────

func (h *handler) deleteDevice(deviceID string) {
	// Remove upload files from disk
	rows, err := h.db.Query(
		`SELECT storage_path FROM full_log_uploads WHERE device_id = $1`, deviceID,
	)
	if err == nil {
		defer rows.Close()
		for rows.Next() {
			var fpath string
			if rows.Scan(&fpath) == nil {
				_ = os.Remove(filepath.Join(h.uploadDir, fpath))
			}
		}
	}

	// Cascade deletes sessions, events, commands, uploads rows
	_, _ = h.db.Exec(`DELETE FROM devices WHERE device_id = $1`, deviceID)
	log.Printf("deleted device %s", deviceID)
}

// ── Cleanup goroutine ─────────────────────────────────────────────────

func (h *handler) runCleanup() {
	ticker := time.NewTicker(6 * time.Hour)
	defer ticker.Stop()
	for range ticker.C {
		// Purge unacknowledged commands older than 7 days
		res, _ := h.db.Exec(`
			DELETE FROM device_commands
			WHERE completed_at IS NULL AND created_at < NOW() - INTERVAL '7 days'
		`)
		if n, _ := res.RowsAffected(); n > 0 {
			log.Printf("cleanup: purged %d stale commands", n)
		}

		// Purge events older than 90 days
		res, _ = h.db.Exec(`
			DELETE FROM events WHERE server_ts < NOW() - INTERVAL '90 days'
		`)
		if n, _ := res.RowsAffected(); n > 0 {
			log.Printf("cleanup: purged %d old events", n)
		}

		// Purge upload records (and files) older than 30 days
		rows, err := h.db.Query(`
			SELECT upload_id, storage_path FROM full_log_uploads
			WHERE uploaded_at < NOW() - INTERVAL '30 days'
		`)
		if err == nil {
			var toDelete []int64
			for rows.Next() {
				var id int64
				var fpath string
				if rows.Scan(&id, &fpath) == nil {
					_ = os.Remove(filepath.Join(h.uploadDir, fpath))
					toDelete = append(toDelete, id)
				}
			}
			rows.Close()
			for _, id := range toDelete {
				_, _ = h.db.Exec(`DELETE FROM full_log_uploads WHERE upload_id = $1`, id)
			}
			if len(toDelete) > 0 {
				log.Printf("cleanup: removed %d old uploads", len(toDelete))
			}
		}
	}
}

// ── Main ──────────────────────────────────────────────────────────────

func main() {
	dbURL := os.Getenv("DATABASE_URL")
	if dbURL == "" {
		log.Fatal("DATABASE_URL not set")
	}
	adminToken := os.Getenv("ADMIN_TOKEN")
	if adminToken == "" {
		log.Fatal("ADMIN_TOKEN not set")
	}
	uploadDir := os.Getenv("UPLOAD_DIR")
	if uploadDir == "" {
		uploadDir = "/data/uploads"
	}
	if err := os.MkdirAll(uploadDir, 0755); err != nil {
		log.Fatalf("cannot create upload dir: %v", err)
	}

	db, err := sql.Open("postgres", dbURL)
	if err != nil {
		log.Fatalf("db open: %v", err)
	}
	db.SetMaxOpenConns(20)
	db.SetMaxIdleConns(5)

	// Wait for postgres to be ready
	for i := 0; i < 30; i++ {
		if err := db.Ping(); err == nil {
			break
		}
		log.Printf("waiting for postgres (%d/30)…", i+1)
		time.Sleep(2 * time.Second)
	}
	if err := db.Ping(); err != nil {
		log.Fatalf("postgres unreachable: %v", err)
	}
	log.Println("connected to postgres")

	h := &handler{db: db, adminToken: adminToken, uploadDir: uploadDir}

	go h.runCleanup()

	mux := http.NewServeMux()

	// Device endpoints
	mux.HandleFunc("POST /api/v1/register",                  h.register)
	mux.HandleFunc("POST /api/v1/sessions",                  h.createSession)
	mux.HandleFunc("POST /api/v1/events",                    h.ingestEvents)
	mux.HandleFunc("GET /api/v1/commands",                   h.getCommands)
	mux.HandleFunc("POST /api/v1/commands/{id}/complete",    h.completeCommand)
	mux.HandleFunc("POST /api/v1/uploads",                   h.upload)
	mux.HandleFunc("POST /api/v1/delete",                    h.deleteSelf)

	// Admin endpoints
	mux.HandleFunc("POST /api/admin/devices/{id}/commands",  h.adminQueueCommand)
	mux.HandleFunc("POST /api/admin/devices/{id}/delete",    h.adminDeleteDevice)
	mux.HandleFunc("GET /api/admin/devices/{id}/summary",    h.adminDeviceSummary)

	// Health probe (used by Caddy depends_on / load balancer)
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) {
		if err := db.Ping(); err != nil {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusOK)
	})

	addr := ":8080"
	log.Printf("listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatalf("server: %v", err)
	}
}
