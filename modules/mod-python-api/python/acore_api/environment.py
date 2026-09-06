from .client import Client
from .model import Action, Observation, transition


class LiveEnvironment:
    """Claim a registered bot. The shared Client can control several bots sequentially."""

    def __init__(self, client: Client, bot: str):
        self.client = client
        self.bot = str(bot)
        self._closed = False
        self.observation = Observation.from_response(client.request("claim", bot=self.bot))

    def reset(self):
        self._ensure_open()
        current = Observation.from_response(self.client.request("reset", bot=self.bot))
        if current.guid != self.bot or current.registration != self.observation.registration:
            raise ValueError("bot registration changed during reset")
        if current.episode != self.observation.episode + 1:
            raise ValueError("reset did not advance the episode")
        self.observation = current
        return current

    def step(self, action: Action, wait_ticks: int = 1):
        self._ensure_open()
        if type(wait_ticks) is not int or not 1 <= wait_ticks <= 100:
            raise ValueError("wait_ticks must be 1..100")
        response = self.client.request(action.operation, bot=self.bot, wait_ticks=wait_ticks, **action.arguments)
        current = Observation.from_response(response)
        result = transition(self.observation, current, int(response["spell_result"]))
        self.observation = current
        return result

    def _ensure_open(self):
        if self._closed:
            raise RuntimeError("environment is closed")

    def close(self):
        if not self._closed:
            self._closed = True
            self.client.request("release", bot=self.bot)

    def __enter__(self):
        return self

    def __exit__(self, error_type, *_):
        if error_type is None:
            self.close()
        else:
            # Preserve the original exception. Disconnect releases all claims server-side.
            self._closed = True
            self.client.close()
