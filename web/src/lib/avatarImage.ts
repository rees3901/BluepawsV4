const MAX_SOURCE_BYTES = 10 * 1024 * 1024;
const AVATAR_SIZE = 256;

export interface AvatarCrop {
  zoom: number;
  offsetX: number;
  offsetY: number;
}

export function validateAvatarFile(file: File) {
  if (!["image/jpeg", "image/png", "image/webp"].includes(file.type)) {
    throw new Error("Choose a JPEG, PNG, or WebP image");
  }
  if (file.size > MAX_SOURCE_BYTES) {
    throw new Error("Choose an image smaller than 10 MB");
  }
}

export async function prepareAvatarImage(file: File, crop: AvatarCrop): Promise<Blob> {
  validateAvatarFile(file);
  const bitmap = await createImageBitmap(file, { imageOrientation: "from-image" });

  try {
    const canvas = document.createElement("canvas");
    canvas.width = AVATAR_SIZE;
    canvas.height = AVATAR_SIZE;
    const context = canvas.getContext("2d");
    if (!context) throw new Error("This browser cannot prepare images");

    const baseScale = Math.max(AVATAR_SIZE / bitmap.width, AVATAR_SIZE / bitmap.height);
    const scale = baseScale * Math.min(3, Math.max(1, crop.zoom));
    const width = bitmap.width * scale;
    const height = bitmap.height * scale;
    const horizontalTravel = Math.max(0, (width - AVATAR_SIZE) / 2);
    const verticalTravel = Math.max(0, (height - AVATAR_SIZE) / 2);
    const x = (AVATAR_SIZE - width) / 2 + horizontalTravel * (crop.offsetX / 100);
    const y = (AVATAR_SIZE - height) / 2 + verticalTravel * (crop.offsetY / 100);

    context.drawImage(bitmap, x, y, width, height);
    const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/webp", 0.86));
    if (!blob) throw new Error("The image could not be prepared");
    return blob;
  } finally {
    bitmap.close();
  }
}
