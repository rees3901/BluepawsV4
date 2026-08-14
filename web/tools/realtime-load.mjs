import { createClient } from "@supabase/supabase-js";

const options = parseArguments(process.argv.slice(2));
const supabaseUrl = requiredEnvironment("SUPABASE_URL");
const publishableKey = requiredEnvironment("SUPABASE_PUBLISHABLE_KEY");
const accessToken = requiredEnvironment("SUPABASE_ACCESS_TOKEN");
const householdId = requiredEnvironment("HOUSEHOLD_ID");
const householdAccessVersion = requiredEnvironment("HOUSEHOLD_ACCESS_VERSION");
const topic = `household:${householdId}:v${householdAccessVersion}`;

const clients = [];
const channels = [];
const latencies = [];
let connected = 0;
let failed = 0;
let messages = 0;

const startedAt = Date.now();
const connectionPromises = Array.from({ length: options.clients }, async (_, index) => {
  const supabase = createClient(supabaseUrl, publishableKey, {
    auth: { autoRefreshToken: false, persistSession: false },
    realtime: { params: { eventsPerSecond: 20 } },
  });
  clients.push(supabase);
  await supabase.realtime.setAuth(accessToken);

  return new Promise((resolve) => {
    let resolved = false;
    const finish = (result) => {
      if (resolved) return;
      resolved = true;
      resolve(result);
    };
    const timeout = setTimeout(() => {
      failed += 1;
      finish("timeout");
    }, options.connectTimeoutSeconds * 1000);

    const channel = supabase
      .channel(topic, { config: { private: true } })
      .on("broadcast", { event: "INSERT" }, receiveMessage)
      .on("broadcast", { event: "UPDATE" }, receiveMessage)
      .subscribe((status) => {
        if (status === "SUBSCRIBED") {
          connected += 1;
          clearTimeout(timeout);
          finish("connected");
        } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
          failed += 1;
          clearTimeout(timeout);
          finish(status.toLowerCase());
        }
      });

    channels[index] = channel;
  });
});

await Promise.all(connectionPromises);
const connectedAt = Date.now();
await delay(options.durationSeconds * 1000);

await Promise.all(channels.map((channel, index) => {
  if (!channel) return Promise.resolve();
  return clients[index].removeChannel(channel);
}));

latencies.sort((left, right) => left - right);
const result = {
  requested_clients: options.clients,
  connected_clients: connected,
  failed_clients: failed,
  connect_seconds: round((connectedAt - startedAt) / 1000),
  observation_seconds: options.durationSeconds,
  messages_received: messages,
  latency_ms: {
    samples: latencies.length,
    p50: percentile(latencies, 0.5),
    p95: percentile(latencies, 0.95),
    p99: percentile(latencies, 0.99),
  },
};

console.log(JSON.stringify(result, null, 2));
process.exitCode = failed === 0 ? 0 : 1;

function receiveMessage(envelope) {
  messages += 1;
  const receivedAt = envelope?.payload?.record?.received_at;
  if (typeof receivedAt !== "string") return;
  const timestamp = Date.parse(receivedAt);
  if (Number.isFinite(timestamp)) latencies.push(Math.max(0, Date.now() - timestamp));
}

function parseArguments(values) {
  const parsed = { clients: 10, durationSeconds: 60, connectTimeoutSeconds: 30 };
  for (let index = 0; index < values.length; index += 2) {
    const key = values[index];
    const value = Number(values[index + 1]);
    if (!Number.isFinite(value) || value <= 0) throw new Error(`Invalid value for ${key}`);
    if (key === "--clients") parsed.clients = Math.floor(value);
    else if (key === "--duration") parsed.durationSeconds = value;
    else if (key === "--connect-timeout") parsed.connectTimeoutSeconds = value;
    else throw new Error(`Unknown argument: ${key}`);
  }
  return parsed;
}

function requiredEnvironment(name) {
  const value = process.env[name];
  if (!value) throw new Error(`Missing ${name}`);
  return value;
}

function percentile(values, fraction) {
  if (values.length === 0) return null;
  return values[Math.min(values.length - 1, Math.floor(values.length * fraction))];
}

function round(value) {
  return Math.round(value * 100) / 100;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
