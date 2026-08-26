import { createClient } from "@/lib/supabase/client";
import type { CustomerPowerProfile } from "@/lib/powerProfiles";

export interface QueuedDeviceCommand {
  id: string;
  device_id: number;
  command_sequence_id: number;
  command_type: "set_profile";
  command_payload: { profile: CustomerPowerProfile };
  status: "pending" | "sent";
  expires_at: string;
}

export async function queuePowerProfileCommand(deviceId: number, profile: CustomerPowerProfile) {
  if (!Number.isInteger(deviceId) || deviceId < 1 || deviceId > 65_535) {
    throw new Error("Invalid collar device ID");
  }

  const supabase = createClient();
  const { data, error } = await supabase.rpc("bluepaws_queue_device_command", {
    requested_device_id: deviceId,
    requested_command_type: "set_profile",
    requested_payload: { profile },
    requested_expires_in: "01:00:00",
  });

  if (error) throw new Error(error.message || "Unable to queue collar command");
  const command = Array.isArray(data) ? data[0] : data;
  if (!command) throw new Error("The command queue returned no command");
  return command as QueuedDeviceCommand;
}
