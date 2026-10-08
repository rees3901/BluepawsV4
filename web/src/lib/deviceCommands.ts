import { createClient } from "@/lib/supabase/client";
import type { CustomerPowerProfile } from "@/lib/powerProfiles";
import { ledFindPayload, type LedFindAction } from "@/lib/ledFind";

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

export async function cancelPowerProfileCommand(commandId: string) {
  const { data, error } = await createClient().rpc("bluepaws_cancel_profile_command", { requested_command_id: commandId });
  if (error) throw new Error(error.message || "Unable to cancel collar command");
  return data === true;
}

export async function queueLedFindCommand(deviceId: number, action: LedFindAction, seconds = 600, interval = 60) {
  if (!Number.isInteger(deviceId) || deviceId < 1 || deviceId >= 65_535 || deviceId % 16 === 0) {
    throw new Error("Invalid collar device ID");
  }
  const { data, error } = await createClient().rpc("bluepaws_queue_device_command", {
    requested_device_id: deviceId,
    requested_command_type: "led_find",
    requested_payload: ledFindPayload(action, seconds, interval),
    requested_expires_in: "00:10:00",
  });
  if (error) throw new Error(error.message || "Unable to queue LED command");
  const command = Array.isArray(data) ? data[0] : data;
  if (!command) throw new Error("The command queue returned no command");
  return command as { id: string; expires_at: string };
}
