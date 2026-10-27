---
name: summarize-document
title: Summarize a document
description: Summarize a long document, report, paper, transcript or web page faithfully, with its key points, numbers and open questions. Use when asked to summarize, digest, give the gist of, or extract the main points from a text.
---

# Summarizing a document

1. **Read all of it.** For a long file, read it in line ranges until the end; for a web page, use
   `fetch_url`. Do not summarize from the first pages only. Note the kind of document (paper,
   contract, meeting notes, specification) and who it is for.
2. **Collect, while reading:** the main claim or decision, the supporting points, every number
   that matters (with its unit and what it measures), dates and deadlines, names of people and
   systems, obligations ("must", "shall"), and what the document says is unknown or open.
3. **Write the summary for the person's purpose.** If they said why they need it, shape it for
   that; otherwise:
   - one or two sentences on what the document is and its main conclusion;
   - five to ten bullet points with the key content, in the document's order;
   - numbers and dates exactly as written, never rounded or recomputed silently;
   - a short "Open questions / caveats" list if the document has any.
4. **Stay faithful.** Do not add facts the document does not contain, and do not soften or
   strengthen its claims. If you add your own judgement, label it as yours. Quote short phrases
   where wording matters (definitions, obligations).
5. **Length:** about a tenth of the original at most, unless asked otherwise.

If the person asked for the summary in a file, write it with `write_file` as Markdown and say
where it is.
