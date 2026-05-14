# Checklist: predicate_demo

## Section: Bool Verify

### Procedure: self verify target
- Action: Enter numeric result
- Spec: 25
- Result:
- Status:
- Comment:

#### Instructions
Type 25 in Result. The BoolVerifyValidatedStatus self-relationship sets this row to Pass when the result matches the numeric spec and Fail when it does not.

#### Relationships
- 9HMX74H40T9ZS5JX
- BoolVerifyValidatedStatus 9HMX74H40T9ZS5JX
- BoolVerifyValidatedStatus ZGQ75MAH9V1QB5FX

### Procedure: downstream target
- Action: Observe propagated status
- Spec: source row controls this status
- Result:
- Status:
- Comment:

#### Instructions
This row receives Pass or Fail from the source row through a BoolVerifyValidatedStatus relationship.

#### Relationships
- ZGQ75MAH9V1QB5FX
