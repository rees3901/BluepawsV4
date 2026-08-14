type FamilyRole = "owner" | "member";

export function canRemoveFamilyMember(
  viewerRole: FamilyRole,
  viewerUserId: string,
  memberRole: FamilyRole,
  memberUserId: string,
) {
  return viewerRole === "owner"
    && memberRole === "member"
    && viewerUserId !== memberUserId;
}
