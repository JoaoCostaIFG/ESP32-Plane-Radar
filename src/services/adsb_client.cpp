#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cfloat>
#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kAdsbRequestTimeoutMs = 10000;
constexpr size_t kEnrichmentCacheSize = 48;

/** Positions remembered per aircraft for trail rendering. */
constexpr size_t kTrailPoints = 8;
constexpr unsigned long kTrailMinStepMs = 4000;
constexpr unsigned long kTrailForgetMs = 120000;

struct TrailHistory {
  char hex[7] = {};
  unsigned long updated_ms = 0;
  size_t count = 0;
  size_t head = 0;
  float lats[kTrailPoints] = {};
  float lons[kTrailPoints] = {};
};

TrailHistory s_trails[kMaxAircraft];

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;
unsigned long s_last_enrichment_lookup_ms = 0;
unsigned long s_last_enrichment_failure_ms = 0;
double s_last_center_lat = 0.0;
double s_last_center_lon = 0.0;

struct EnrichmentCacheEntry {
  char hex[7] = {};
  char callsign[9] = {};
  char route[10] = {};
  char type[18] = {};
  unsigned long refreshed_ms = 0;
  bool has_data = false;
};

EnrichmentCacheEntry s_enrichment_cache[kEnrichmentCacheSize];

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http, unsigned long timeout_ms) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long started_ms = millis();
  while (millis() - started_ms < timeout_ms) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload,
                              unsigned long timeout_ms) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long started_ms = millis();
  while (millis() - started_ms < timeout_ms) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  ac->route[0] = '\0';
  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  float baro_rate = 0.0f;
  if (readJsonFloat(plane, "baro_rate", &baro_rate)) {
    ac->baro_rate_fpm = static_cast<int>(lroundf(baro_rate));
  } else {
    ac->baro_rate_fpm = 0;
  }
}

bool sameKey(const EnrichmentCacheEntry& entry, const Aircraft& plane) {
  return entry.refreshed_ms != 0 && strcmp(entry.hex, plane.hex) == 0 &&
         strcmp(entry.callsign, plane.callsign) == 0;
}

bool cacheEntryFresh(const EnrichmentCacheEntry& entry,
                     unsigned long now) {
  if (entry.refreshed_ms == 0) {
    return false;
  }
  const unsigned long ttl = entry.has_data ? config::kFlightCacheSuccessMs
                                           : config::kFlightCacheMissMs;
  return now - entry.refreshed_ms < ttl;
}

EnrichmentCacheEntry* findCacheEntry(const Aircraft& plane,
                                     unsigned long now) {
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) && cacheEntryFresh(entry, now)) {
      return &entry;
    }
  }
  return nullptr;
}

bool copyIfDifferent(char* destination, size_t destination_len,
                     const char* source) {
  if (source == nullptr || source[0] == '\0' ||
      strcmp(destination, source) == 0) {
    return false;
  }
  strncpy(destination, source, destination_len - 1);
  destination[destination_len - 1] = '\0';
  return true;
}

bool applyCacheEntry(Aircraft* plane, const EnrichmentCacheEntry& entry) {
  bool changed = false;
  changed |= copyIfDifferent(plane->route, sizeof(plane->route), entry.route);
  changed |= copyIfDifferent(plane->type, sizeof(plane->type), entry.type);
  return changed;
}

EnrichmentCacheEntry* cacheSlotFor(const Aircraft& plane,
                                   unsigned long now) {
  EnrichmentCacheEntry* oldest = &s_enrichment_cache[0];
  unsigned long oldest_age = 0;
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) || entry.refreshed_ms == 0) {
      return &entry;
    }
    const unsigned long age = now - entry.refreshed_ms;
    if (age >= oldest_age) {
      oldest = &entry;
      oldest_age = age;
    }
  }
  return oldest;
}

void copySafeIdentifier(const char* source, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  size_t written = 0;
  if (source != nullptr) {
    for (size_t i = 0; source[i] != '\0' && written + 1 < out_len; ++i) {
      const unsigned char ch = static_cast<unsigned char>(source[i]);
      if (std::isalnum(ch)) {
        out[written++] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
    }
  }
  out[written] = '\0';
}

void copyAirportCode(const JsonObject& airport, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  copyJsonStringTrimmed(airport, "iata_code", out, out_len);
  if (out[0] == '\0') {
    copyJsonStringTrimmed(airport, "icao_code", out, out_len);
  }
}

void parseRoute(const JsonObject& response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject route = response["flightroute"].as<JsonObject>();
  if (route.isNull()) {
    return;
  }

  char origin[5] = {};
  char destination[5] = {};
  copyAirportCode(route["origin"].as<JsonObject>(), origin, sizeof(origin));
  copyAirportCode(route["destination"].as<JsonObject>(), destination,
                  sizeof(destination));
  if (origin[0] != '\0' && destination[0] != '\0') {
    snprintf(out, out_len, "%s-%s", origin, destination);
  }
}

bool startsWith(const char* value, const char* prefix) {
  return value != nullptr && prefix != nullptr &&
         strncmp(value, prefix, strlen(prefix)) == 0;
}

void compactAircraftType(const char* detailed, const char* icao, char* out,
                         size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';

  const char* value = detailed;
  char prefixed[40] = {};
  if (startsWith(value, "Boeing ")) {
    snprintf(prefixed, sizeof(prefixed), "B%s", value + 7);
    value = prefixed;
  } else if (startsWith(value, "Airbus ")) {
    value += 7;
  } else if (startsWith(value, "Bombardier ")) {
    value += 11;
  } else if (startsWith(value, "Embraer ")) {
    value += 8;
  } else if (startsWith(value, "De Havilland Canada ")) {
    value += 20;
  }

  if (value == nullptr || value[0] == '\0') {
    value = icao;
  }
  if (value == nullptr) {
    return;
  }

  size_t written = 0;
  bool previous_space = false;
  for (size_t i = 0; value[i] != '\0' && written + 1 < out_len; ++i) {
    const char ch = value[i];
    if (ch == ' ') {
      if (!previous_space) {
        out[written++] = ' ';
        previous_space = true;
      }
      continue;
    }
    out[written++] = ch;
    previous_space = false;
  }
  while (written > 0 && out[written - 1] == ' ') {
    --written;
  }
  out[written] = '\0';
}

void parseDetailedType(const JsonObject& response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject aircraft = response["aircraft"].as<JsonObject>();
  if (aircraft.isNull()) {
    return;
  }
  compactAircraftType(aircraft["type"] | nullptr,
                      aircraft["icao_type"] | nullptr, out, out_len);
}

String enrichmentUrl(const Aircraft& plane) {
  char hex[sizeof(plane.hex)] = {};
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.hex, hex, sizeof(hex));
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));

  String url = config::kFlightDataApiBase;
  if (hex[0] != '\0') {
    url += "aircraft/";
    url += hex;
    if (callsign[0] != '\0' && strcmp(hex, callsign) != 0) {
      url += "?callsign=";
      url += callsign;
    }
  } else {
    url += "callsign/";
    url += callsign;
  }
  return url;
}

String callsignRouteUrl(const Aircraft& plane) {
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));
  if (callsign[0] == '\0') {
    return {};
  }

  String url = config::kFlightDataApiBase;
  url += "callsign/";
  url += callsign;
  return url;
}

bool fetchFlightDataJson(const String& url, const char* callsign,
                         JsonDocument* doc, bool* not_found) {
  *not_found = false;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("flight data: http.begin failed");
    return false;
  }
  http.setTimeout(config::kFlightLookupTimeoutMs);
  const int code = performGetWithPoll(http, config::kFlightLookupTimeoutMs);

  String payload;
  if (code == HTTP_CODE_OK) {
    readResponseBodyWithPoll(http, payload, config::kFlightLookupTimeoutMs);
  }
  http.end();

  if (code == HTTP_CODE_NOT_FOUND) {
    *not_found = true;
    return true;
  }
  if (code != HTTP_CODE_OK || payload.length() == 0) {
    Serial.printf("flight data: HTTP %d for %s\n", code, callsign);
    return false;
  }

  const DeserializationError error = deserializeJson(*doc, payload);
  if (error) {
    Serial.printf("flight data: JSON parse error: %s\n", error.c_str());
    return false;
  }
  return true;
}

bool fetchEnrichment(const Aircraft& plane, EnrichmentCacheEntry* entry) {
  entry->route[0] = '\0';
  entry->type[0] = '\0';

  JsonDocument doc;
  bool not_found = false;
  if (!fetchFlightDataJson(enrichmentUrl(plane), plane.callsign, &doc,
                           &not_found)) {
    return false;
  }

  if (!not_found) {
    JsonObject response = doc["response"].as<JsonObject>();
    if (!response.isNull()) {
      parseRoute(response, entry->route, sizeof(entry->route));
      parseDetailedType(response, entry->type, sizeof(entry->type));
    }
  }

  // ADSBDB may return HTTP 200 "unknown aircraft" from the combined endpoint
  // even though its callsign database has a valid route. Retry only the route
  // through the callsign endpoint in that case.
  if (entry->route[0] == '\0') {
    const String route_url = callsignRouteUrl(plane);
    if (route_url.length() > 0 && route_url != enrichmentUrl(plane)) {
      JsonDocument route_doc;
      bool route_not_found = false;
      if (!fetchFlightDataJson(route_url, plane.callsign, &route_doc,
                               &route_not_found)) {
        return false;
      }
      if (!route_not_found) {
        JsonObject response = route_doc["response"].as<JsonObject>();
        if (!response.isNull()) {
          parseRoute(response, entry->route, sizeof(entry->route));
        }
      }
    }
  }

  if (entry->route[0] == '\0' && entry->type[0] == '\0') {
    Serial.printf("flight data: no match for %s\n", plane.callsign);
  }
  return true;
}

void applyCachedEnrichment(Aircraft* plane) {
  EnrichmentCacheEntry* entry = findCacheEntry(*plane, millis());
  if (entry != nullptr) {
    applyCacheEntry(plane, *entry);
  }
}

TrailHistory* trailSlotFor(const Aircraft& plane, unsigned long now) {
  TrailHistory* free_slot = nullptr;
  TrailHistory* stale_slot = nullptr;
  unsigned long stale_age = 0;
  for (auto& trail : s_trails) {
    if (trail.hex[0] == '\0') {
      if (free_slot == nullptr) {
        free_slot = &trail;
      }
      continue;
    }
    if (strcmp(trail.hex, plane.hex) == 0) {
      if (now - trail.updated_ms > kTrailForgetMs) {
        trail.count = 0;
        trail.head = 0;
      }
      return &trail;
    }
    const unsigned long age = now - trail.updated_ms;
    if (age > kTrailForgetMs && age > stale_age) {
      stale_slot = &trail;
      stale_age = age;
    }
  }
  if (stale_slot != nullptr) {
    stale_slot->hex[0] = '\0';
    stale_slot->count = 0;
    stale_slot->head = 0;
    return stale_slot;
  }
  if (free_slot != nullptr) {
    strncpy(free_slot->hex, plane.hex, sizeof(free_slot->hex) - 1);
    free_slot->hex[sizeof(free_slot->hex) - 1] = '\0';
    return free_slot;
  }
  return nullptr;
}

void recordTrailPoint(Aircraft* plane, const JsonObject& json) {
  if (plane->hex[0] == '\0') {
    return;
  }
  float lat = 0.0f;
  float lon = 0.0f;
  if (!readJsonFloat(json, "lat", &lat) || !readJsonFloat(json, "lon", &lon)) {
    return;
  }

  const unsigned long now = millis();
  TrailHistory* trail = trailSlotFor(*plane, now);
  if (trail == nullptr) {
    return;
  }

  if (trail->count > 0 && now - trail->updated_ms < kTrailMinStepMs) {
    return;
  }

  trail->lats[trail->head] = plane->lat;
  trail->lons[trail->head] = plane->lon;
  trail->head = (trail->head + 1) % kTrailPoints;
  if (trail->count < kTrailPoints) {
    ++trail->count;
  }
  trail->updated_ms = now;
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t trailPointsFor(const char* hex, float* lats, float* lons,
                      size_t max_points) {
  if (hex == nullptr || hex[0] == '\0') {
    return 0;
  }
  const unsigned long now = millis();
  for (const auto& trail : s_trails) {
    if (trail.hex[0] == '\0' || strcmp(trail.hex, hex) != 0) {
      continue;
    }
    if (now - trail.updated_ms > kTrailForgetMs || trail.count == 0) {
      return 0;
    }
    size_t n = trail.count < max_points ? trail.count : max_points;
    // Ring buffer: head marks the next write; oldest entry follows it.
    size_t index = (trail.head + kTrailPoints - trail.count) % kTrailPoints;
    for (size_t i = 0; i < n; ++i) {
      lats[i] = trail.lats[index];
      lons[i] = trail.lons[index];
      index = (index + 1) % kTrailPoints;
    }
    return n;
  }
  return 0;
}

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  s_last_center_lat = center_lat;
  s_last_center_lon = center_lon;
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kAdsbRequestTimeoutMs);
  const int code = performGetWithPoll(http, kAdsbRequestTimeoutMs);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readResponseBodyWithPoll(http, payload, kAdsbRequestTimeoutMs)) {
    Serial.println("adsb: empty response");
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    applyCachedEnrichment(&s_aircraft[n]);
    recordTrailPoint(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

bool enrichOnePending() {
  const unsigned long now = millis();
  if (s_aircraft_count == 0 ||
      (s_last_enrichment_failure_ms != 0 &&
       now - s_last_enrichment_failure_ms <
           config::kFlightLookupFailureBackoffMs) ||
      (s_last_enrichment_lookup_ms != 0 &&
       now - s_last_enrichment_lookup_ms <
           config::kFlightLookupMinIntervalMs)) {
    return false;
  }

  size_t candidate_index = kMaxAircraft;
  float candidate_dist_sq = FLT_MAX;
  for (size_t index = 0; index < s_aircraft_count; ++index) {
    Aircraft& plane = s_aircraft[index];
    if (plane.hex[0] == '\0' && plane.callsign[0] == '\0') {
      continue;
    }

    EnrichmentCacheEntry* cached = findCacheEntry(plane, now);
    if (cached != nullptr) {
      applyCacheEntry(&plane, *cached);
      continue;
    }

    const float dlat = plane.lat - static_cast<float>(s_last_center_lat);
    const float dlon = plane.lon - static_cast<float>(s_last_center_lon);
    const float dist_sq = dlat * dlat + dlon * dlon;
    if (dist_sq < candidate_dist_sq) {
      candidate_index = index;
      candidate_dist_sq = dist_sq;
    }
  }

  if (candidate_index == kMaxAircraft) {
    return false;
  }

  Aircraft& plane = s_aircraft[candidate_index];
  s_last_enrichment_lookup_ms = now;
  EnrichmentCacheEntry* entry = cacheSlotFor(plane, now);
  *entry = {};
  strncpy(entry->hex, plane.hex, sizeof(entry->hex) - 1);
  strncpy(entry->callsign, plane.callsign, sizeof(entry->callsign) - 1);

  if (!fetchEnrichment(plane, entry)) {
    // Network failures are retried after a short global backoff.
    *entry = {};
    s_last_enrichment_failure_ms = millis();
    return false;
  }

  s_last_enrichment_failure_ms = 0;
  entry->refreshed_ms = millis();
  entry->has_data = entry->route[0] != '\0' || entry->type[0] != '\0';
  const bool changed = applyCacheEntry(&plane, *entry);
  Serial.printf("flight data: %s %s %s\n", plane.callsign,
                entry->route[0] != '\0' ? entry->route : "(route unknown)",
                entry->type[0] != '\0' ? entry->type : "(type unchanged)");
  return changed;
}

}  // namespace services::adsb
