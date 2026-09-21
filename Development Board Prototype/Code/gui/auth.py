"""Password verification and atomic local credential persistence."""

import hashlib
import hmac
import json
import secrets
from pathlib import Path


PASSWORD_KDF_ITERATIONS = 200000


class PasswordAuth:
    """Verify passwords and atomically persist only salted PBKDF2 hashes."""

    def __init__(self, env_password: str | None, default_password: str, store_path: Path) -> None:
        self._env_password = env_password
        self._default_password = default_password
        self._store_path = store_path
        self._salt_hex = None
        self._hash_hex = None
        self._load_store()

    def _load_store(self) -> None:
        """Load a valid credential record; malformed files fall back safely."""
        if not self._store_path.exists():
            return
        try:
            payload = json.loads(self._store_path.read_text(encoding="utf-8"))
            salt_hex = payload.get("salt_hex")
            hash_hex = payload.get("hash_hex")
            if isinstance(salt_hex, str) and isinstance(hash_hex, str):
                self._salt_hex = salt_hex
                self._hash_hex = hash_hex
        except (OSError, ValueError, TypeError):
            self._salt_hex = None
            self._hash_hex = None

    @staticmethod
    def _hash_password(password: str, salt_hex: str) -> str:
        salt = bytes.fromhex(salt_hex)
        digest = hashlib.pbkdf2_hmac(
            "sha256",
            password.encode("utf-8"),
            salt,
            PASSWORD_KDF_ITERATIONS,
        )
        return digest.hex()

    def verify(self, candidate: str) -> bool:
        """Compare against the environment override, saved hash, or default."""
        if self._env_password is not None:
            return hmac.compare_digest(candidate, self._env_password)
        if self._salt_hex is not None and self._hash_hex is not None:
            candidate_hash = self._hash_password(candidate, self._salt_hex)
            return hmac.compare_digest(candidate_hash, self._hash_hex)
        return hmac.compare_digest(candidate, self._default_password)

    def can_change(self) -> bool:
        return self._env_password is None

    @staticmethod
    def cannot_change_reason() -> str:
        return "Password changes are disabled while RTC_GUI_PASSWORD is set in the environment."

    def change_password(self, old_password: str, new_password: str) -> tuple[bool, str]:
        """Validate the old password and atomically replace the stored hash.

        The temporary-file replacement prevents power loss or process failure
        from leaving a partially written credential record.
        """
        if not self.can_change():
            return False, self.cannot_change_reason()
        if not self.verify(old_password):
            return False, "Current password is incorrect."

        salt_hex = secrets.token_hex(16)
        payload = {
            "salt_hex": salt_hex,
            "hash_hex": self._hash_password(new_password, salt_hex),
            "kdf": "pbkdf2_hmac_sha256",
            "iterations": PASSWORD_KDF_ITERATIONS,
        }
        try:
            temp_path = self._store_path.with_suffix(self._store_path.suffix + ".tmp")
            temp_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
            temp_path.replace(self._store_path)
        except OSError as exc:
            return False, f"Failed to save password: {exc}"

        self._salt_hex = payload["salt_hex"]
        self._hash_hex = payload["hash_hex"]
        return True, "Password updated successfully."
