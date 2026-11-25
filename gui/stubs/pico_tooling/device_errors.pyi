class RefusalError(Exception):
    status: int
    code: str | None
    def __init__(
        self,
        status: int,
        message: str,
        *,
        code: str | None = None,
    ) -> None: ...
