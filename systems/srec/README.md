# SRec

SRec is a reserved ServerlessX subsystem boundary for recovery semantics:
checkpoint, restart, retry, replay, and recovery evidence. This bootstrap does
not include an SRec implementation or a runnable profile.

The directory exists so future recovery code can be added without confusing
research notes with the portable SPD implementation. Any future SRec feature
must define its state format, failure model, ownership rules, and verification
evidence before being advertised as executable.
