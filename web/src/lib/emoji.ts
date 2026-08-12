const GOOGLE_EMOJI_CDN = "https://cdn.jsdelivr.net/npm/emoji-datasource-google/img/google/64";

export function emojiToUnified(emoji: string) {
  return Array.from(emoji, (character) => character.codePointAt(0)?.toString(16)).filter(Boolean).join("-");
}

export function emojiImageUrl(emoji: string) {
  return `${GOOGLE_EMOJI_CDN}/${emojiToUnified(emoji)}.png`;
}
