#include "meshsat/Smaz2.h"

#include <cstring>

namespace meshsat
{

// Generated from meshsat-hub internal/compress/smaz2.go on 27 Sep 2026; keep in step with the Hub.
static const char BIGRAMS[] = "intherreheanonesorteattistenntartondalitseediseangoulecomeneriro"
                              "deraioicliofasetvetasihamaecomceelllcaurlachhidihofonsotacnarsso"
                              "prrtsassusnoiltsemctgeloeebetrnipeiepancpooldaadviunamutwimoshyo"
                              "aiewowosfiepttmiopiaweagsuiddoooirspplscaywaigeirylytuulivimabty";
static constexpr size_t BIGRAM_COUNT = (sizeof(BIGRAMS) - 1) / 2;

static const char *const WORDS[256] = {
    "mesh",        "node",        "message",      "channel",       "signal",      "radio",       "frequency",  "power",
    "antenna",     "range",       "gateway",      "relay",         "bridge",      "network",     "packet",     "payload",
    "status",      "battery",     "voltage",      "level",         "percent",     "position",    "latitude",   "longitude",
    "altitude",    "heading",     "speed",        "distance",      "bearing",     "elevation",   "terrain",    "route",
    "satellite",   "iridium",     "modem",        "serial",        "port",        "device",      "sensor",     "timestamp",
    "meshtastic",  "location",    "coordinates",  "waypoint",      "temperature", "humidity",    "pressure",   "weather",
    "connected",   "offline",     "online",       "update",        "received",    "transmitted", "error",      "warning",
    "critical",    "enabled",     "disabled",     "configuration", "settings",    "interface",   "protocol",   "timeout",
    "emergency",   "rescue",      "medical",      "assistance",    "helicopter",  "vehicle",     "boat",       "shelter",
    "water",       "food",        "camp",         "base",          "standing",    "moving",      "stopped",    "request",
    "respond",     "acknowledge", "confirmed",    "denied",        "approved",    "copy",        "over",       "roger",
    "affirmative", "negative",    "clear",        "blocked",       "north",       "south",       "east",       "west",
    "success",     "failed",      "pending",      "queued",        "delivered",   "expired",     "forwarded",  "dropped",
    "retry",       "noise",       "interference", "coverage",      "degrees",     "meters",      "kilometers", "knots",
    "duration",    "interval",    "threshold",    "maximum",       "minimum",     "average",     "total",      "count",
    "report",      "checksum",    "baud",         "cellular",      "globalstar",  "webhook",     "mqtt",       "iridium",
    "that",        "this",        "with",         "from",          "your",        "have",        "more",       "will",
    "home",        "about",       "time",         "they",          "what",        "which",       "their",      "there",
    "only",        "when",        "here",         "also",          "help",        "been",        "would",      "were",
    "some",        "these",       "like",         "than",          "find",        "back",        "just",       "over",
    "into",        "them",        "should",       "then",          "good",        "well",        "where",      "right",
    "high",        "through",     "each",         "very",          "read",        "need",        "many",       "said",
    "does",        "under",       "full",         "part",          "could",       "great",       "send",       "type",
    "because",     "local",       "those",        "using",         "result",      "before",      "make",       "data",
    "area",        "want",        "show",         "even",          "check",       "open",        "today",      "state",
    "both",        "down",        "system",       "three",         "total",       "place",       "without",    "access",
    "think",       "current",     "control",      "change",        "small",       "rate",        "number",     "line",
    "name",        "list",        "work",         "last",          "next",        "used",        "free",       "other",
    "first",       "world",       "after",        "best",          "long",        "based",       "code",       "being",
    "while",       "care",        "same",         "found",         "link",        "text",        "site",       "form",
    "event",       "love",        "main",         "still",         "information", "service",     "people",     "year",
    "email",       "group",       "detail",       "design",        "board",       "point",       "test",       "track",
};

static bool put(uint8_t *out, size_t capacity, size_t &n, const char *s, size_t len)
{
    if (n + len > capacity)
        return false;
    memcpy(out + n, s, len);
    n += len;
    return true;
}

size_t smaz2Decompress(const uint8_t *in, size_t length, uint8_t *out, size_t capacity)
{
    size_t n = 0;
    size_t i = 0;
    while (i < length) {
        const uint8_t b = in[i];
        if (b >= 128) {
            const size_t idx = b & 0x7F;
            if (idx >= BIGRAM_COUNT || !put(out, capacity, n, BIGRAMS + idx * 2, 2))
                return 0;
            i++;
        } else if (b >= 1 && b <= 5) {
            const size_t run = b;
            if (i + 1 + run > length || !put(out, capacity, n, reinterpret_cast<const char *>(in + i + 1), run))
                return 0;
            i += 1 + run;
        } else if (b == 6 || b == 7 || b == 8) {
            if (i + 1 >= length)
                return 0;
            const char *word = WORDS[in[i + 1]];
            if (b == 8 && !put(out, capacity, n, " ", 1))
                return 0;
            if (!put(out, capacity, n, word, strlen(word)))
                return 0;
            if (b == 7 && !put(out, capacity, n, " ", 1))
                return 0;
            i += 2;
        } else {
            const char literal = static_cast<char>(b);
            if (!put(out, capacity, n, &literal, 1))
                return 0;
            i++;
        }
    }
    return n;
}

} // namespace meshsat
