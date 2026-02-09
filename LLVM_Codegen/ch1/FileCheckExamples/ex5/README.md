This example shows how to use variables in FileCheck.

Variables allow to capture patterns (regex) then reuse that pattern in a later
FileCheck commands.
Variables use the `[[variable]]` delimiters.
They are defined with `[[variable:regex]]` and used via `[[variable]]`.
