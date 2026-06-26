# Local Onboarding

This firmware ports the core Hao Lab onboarding idea into the standalone StackChan M5 build.

The original `haolab.ai` onboarding flow has three server-side roles:

- Interviewer: asks short, warm questions.
- Watcher: extracts structured fields from the conversation.
- Creator: writes the final agent profile.

This device build keeps the useful interaction pattern but runs locally:

- The Agent calls local MCP tools when the user says `onboarding`.
- The M5 asks one onboarding question at a time.
- Answers are saved in NVS under namespace `onboard`.
- A compact user preference summary is persisted and can be retrieved by the Agent later.

## MCP Tools

- `self.onboarding.start`
- `self.onboarding.record_answer`
- `self.onboarding.get_profile`
- `self.onboarding.reset`

When the user says `onboarding`, the Agent should call `self.onboarding.start`, ask the returned `next_question`, then call `self.onboarding.record_answer` after each user answer.

After all questions are answered, `self.onboarding.record_answer` returns `complete: true` and a `summary`. The Agent should use that summary for future tone, detail level, examples, and water reminder style.

`self.onboarding.get_profile` returns the persisted profile after reboot.

## Questions

The local flow asks:

1. What should I call you?
2. Do you prefer short direct answers or more explanation?
3. What do you most often want me to pay attention to?
4. Should I feel more like an assistant, friend, or action partner?
5. For water reminders, do you prefer a gentle or direct style?

## Validation

After flashing, say:

```text
onboarding
```

Then answer each question. When onboarding completes, ask a normal question and the Agent can call `self.onboarding.get_profile` to adapt the reply.

To clear the saved profile, say:

```text
reset my onboarding profile
```
