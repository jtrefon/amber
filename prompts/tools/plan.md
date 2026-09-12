## todowrite

A structured task list for the session, maintained by you and visible to the
user. Each item has an id, a short description, and a status (`pending`,
`in_progress`, `completed`, `cancelled`). Send the **full updated list** on
every call — it replaces the previous one. The list lives outside the
conversation, so it survives context compaction and keeps you and the user
on the same page across long tasks.

A task list earns its keep when the work has shape: three or more distinct
steps, steps that depend on earlier ones, files that change together, or new
instructions arriving mid-task. Those are the moments to write the list down
and keep it current — it turns scattered effort into a visible path, and it
gives the user a window into where the work stands. A fresh list of 3–5
actionable items, marked `in_progress` while active and `completed` when
done, is the natural shape for this tool. When the task is a single quick
step, the list stays quiet — tracking adds nothing there.
