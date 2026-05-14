# Checklist: prefill_demo

## Section: CSV Prefill

### Procedure: lookup key
- Action: Enter run key
- Spec: run-001
- Result:
- Status:
- Comment:

#### Instructions
Type run-001 in Result. The ResultSearchPrefill relationships use data/prefill_demo.csv to fill the downstream rows.

#### Relationships
- 1S1BP2CZQ654BE7V
- ResultSearchPrefillResult CVKQACGDNY06RTC3
- ResultSearchPrefillResult 4YXQP72CVXSFC0AK
- ResultSearchPrefillComment 6F6CQM64Q10EC6ZD

### Procedure: captured temperature
- Action: Read filled temperature
- Spec: temperature populated
- Result:
- Status:
- Comment:

#### Instructions
This Result field is filled from the CSV row matching the lookup key.

#### Relationships
- CVKQACGDNY06RTC3

### Procedure: captured pressure
- Action: Read filled pressure
- Spec: pressure populated
- Result:
- Status:
- Comment:

#### Instructions
This Result field is filled from the CSV row matching the lookup key.

#### Relationships
- 4YXQP72CVXSFC0AK

### Procedure: captured note
- Action: Read filled note
- Spec: note populated
- Result:
- Status:
- Comment:

#### Instructions
This Comment field is filled from the CSV row matching the lookup key.

#### Relationships
- 6F6CQM64Q10EC6ZD
