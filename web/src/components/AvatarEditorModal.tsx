"use client";

import { useEffect, useState, type CSSProperties } from "react";
import EmojiPicker, { Emoji, EmojiStyle, Theme, type EmojiClickData } from "emoji-picker-react";
import { prepareAvatarImage, validateAvatarFile } from "@/lib/avatarImage";
import { saveDeviceAppearance } from "@/lib/deviceAppearances";
import { emojiToUnified } from "@/lib/emoji";
import { normalizeMarkerColor } from "@/lib/markerColor";
import type { DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

const SUGGESTED_COLORS = ["#1d9bf0", "#ff6b35", "#a855f7", "#22c55e", "#f59e0b", "#06b6d4", "#84cc16", "#ec4899"];

interface AvatarEditorModalProps {
  device: TelemetryDevice;
  householdId: string;
  avatar: DeviceAvatar;
  theme: "dark" | "light";
  onClose: () => void;
  onSaved: () => Promise<void>;
}

export function AvatarEditorModal({ device, householdId, avatar, theme, onClose, onSaved }: AvatarEditorModalProps) {
  const [kind, setKind] = useState<"emoji" | "photo">(avatar.kind);
  const [emoji, setEmoji] = useState(avatar.emoji);
  const [color, setColor] = useState(() => normalizeMarkerColor(avatar.color));
  const [file, setFile] = useState<File | null>(null);
  const [previewUrl, setPreviewUrl] = useState<string | null>(null);
  const [zoom, setZoom] = useState(1);
  const [offsetX, setOffsetX] = useState(0);
  const [offsetY, setOffsetY] = useState(0);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    return () => {
      if (previewUrl) URL.revokeObjectURL(previewUrl);
    };
  }, [previewUrl]);

  const selectFile = (selected: File | undefined) => {
    if (!selected) return;
    try {
      validateAvatarFile(selected);
      if (previewUrl) URL.revokeObjectURL(previewUrl);
      setFile(selected);
      setPreviewUrl(URL.createObjectURL(selected));
      setZoom(1);
      setOffsetX(0);
      setOffsetY(0);
      setKind("photo");
      setError(null);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The image could not be selected");
    }
  };

  const save = async () => {
    setSaving(true);
    setError(null);
    try {
      const preparedPhoto = kind === "photo" && file
        ? await prepareAvatarImage(file, { zoom, offsetX, offsetY })
        : undefined;
      await saveDeviceAppearance({
        deviceId: device.id,
        householdId,
        kind,
        emoji,
        color,
        previousStoragePath: avatar.storagePath,
        preparedPhoto,
      });
      await onSaved();
      onClose();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The appearance could not be saved");
    } finally {
      setSaving(false);
    }
  };

  const photoPreview = previewUrl ?? avatar.photoUrl;
  return (
    <div className="modal avatar-editor" role="dialog" aria-modal="true" aria-labelledby="avatar-editor-title" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
      <div className="modal-content avatar-editor-content">
        <div className="avatar-editor-heading">
          <div>
            <span className="avatar-editor-eyebrow">Pet marker</span>
            <h2 id="avatar-editor-title">Customise {device.name}</h2>
          </div>
          <button type="button" className="avatar-editor-close" aria-label="Close avatar editor" onClick={onClose}>×</button>
        </div>

        <div className="avatar-kind-tabs" role="tablist" aria-label="Avatar type">
          <button type="button" role="tab" aria-selected={kind === "emoji"} className={kind === "emoji" ? "active" : ""} onClick={() => setKind("emoji")}>Emoji</button>
          <button type="button" role="tab" aria-selected={kind === "photo"} className={kind === "photo" ? "active" : ""} onClick={() => setKind("photo")}>Photo</button>
        </div>

        {kind === "emoji" ? (
          <div className="avatar-emoji-picker">
            <div className="avatar-selected-emoji" aria-live="polite">
              <span className="marker-pin avatar-map-pin-preview" style={{ "--marker-color": color } as CSSProperties}>
                <span className="card-avatar marker-pin-face">
                  <Emoji unified={emojiToUnified(emoji)} emojiStyle={EmojiStyle.GOOGLE} size={25} />
                </span>
              </span>
              <span><strong>Selected marker</strong><small>Search below or browse by category</small></span>
            </div>
            <EmojiPicker
              width="100%"
              height={340}
              theme={theme === "dark" ? Theme.DARK : Theme.LIGHT}
              emojiStyle={EmojiStyle.GOOGLE}
              searchPlaceholder="Search emojis and symbols"
              lazyLoadEmojis
              previewConfig={{ showPreview: false }}
              onEmojiClick={(choice: EmojiClickData) => setEmoji(choice.emoji)}
            />
          </div>
        ) : (
          <div className="avatar-photo-editor">
            <div
              className="avatar-photo-preview"
              style={{ borderColor: color }}
              aria-label="Photo crop preview"
            >
              {photoPreview ? (
                <span
                  className="avatar-photo-preview-image"
                  style={{
                    backgroundImage: `url(${JSON.stringify(photoPreview)})`,
                    transform: `scale(${zoom}) translate(${offsetX / 6}%, ${offsetY / 6}%)`,
                  }}
                />
              ) : <span className="avatar-photo-placeholder">📷</span>}
            </div>
            <label className="avatar-upload-button">
              <input type="file" accept="image/jpeg,image/png,image/webp" onChange={(event) => selectFile(event.target.files?.[0])} />
              {photoPreview ? "Choose another photo" : "Choose a photo"}
            </label>
            <p className="form-hint">JPEG, PNG or WebP, up to 10 MB. Bluepaws stores a private, metadata-free square copy.</p>
            {file && (
              <div className="avatar-crop-controls">
                <label>Zoom <input type="range" min="1" max="3" step="0.05" value={zoom} onChange={(event) => setZoom(Number(event.target.value))} /></label>
                <label>Left / right <input type="range" min="-100" max="100" value={offsetX} onChange={(event) => setOffsetX(Number(event.target.value))} /></label>
                <label>Up / down <input type="range" min="-100" max="100" value={offsetY} onChange={(event) => setOffsetY(Number(event.target.value))} /></label>
              </div>
            )}
          </div>
        )}

        <div className="avatar-colour-section">
          <div className="avatar-colour-heading">
            <span>Marker colour</span>
            <output aria-live="polite">{color.toUpperCase()}</output>
          </div>
          <div className="avatar-colour-controls">
            <div className="avatar-colour-grid" aria-label="Suggested marker colours">
              {SUGGESTED_COLORS.map((option) => (
                <button key={option} type="button" style={{ backgroundColor: option, color: option }} className={color === option ? "active" : ""} aria-label={`Use suggested marker colour ${option}`} aria-pressed={color === option} onClick={() => setColor(option)} />
              ))}
            </div>
            <label className="avatar-custom-colour">
              <input type="color" value={color} aria-label="Choose any custom marker colour" onInput={(event) => setColor(event.currentTarget.value)} />
              <span><strong>Custom colour</strong><small>Open the full colour picker</small></span>
            </label>
          </div>
        </div>

        {error && <p className="avatar-editor-error" role="alert">{error}</p>}
        <div className="modal-actions">
          <button className="btn-primary" type="button" disabled={saving || (kind === "photo" && !photoPreview)} onClick={save}>{saving ? "Saving…" : "Save appearance"}</button>
          <button className="btn-secondary" type="button" disabled={saving} onClick={onClose}>Cancel</button>
        </div>
      </div>
    </div>
  );
}
