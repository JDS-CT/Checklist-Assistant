# Checklist: initial_setup

## Section: First Run

### Procedure: create startup instance
- Action: Create or select the startup instance
- Spec: Instance is startup
- Result: 
- Status: 
- Comment: 

#### Instructions
Use the Instance search box above the table, type `startup`, press Enter, and confirm creation if prompted; this gives your first settings checklist a stable local instance.

### Procedure: identify user
- Action: Enter your display name
- Spec: Name captured
- Result: 
- Status: 
- Comment: 

#### Instructions
Enter the name you want humans and assistants to use when they talk about this workstation or operator, then mark the row Pass when it looks right.

### Procedure: choose checklist asset root
- Action: Enter the path to an extra checklist asset folder
- Spec: Path captured
- Result: 
- Status: 
- Comment: 

#### Instructions
Enter a local folder that either is named `checklists` or contains a `checklists` folder; private or stakeholder-specific assets should live outside the public repo and be loaded through Portal Settings.

### Procedure: save asset root
- Action: Open Portal Settings and save the asset root
- Spec: Effective roots include the folder
- Result: 
- Status: 
- Comment: 

#### Instructions
Open Settings, add one line in Checklist asset roots using `private_assets=PATH`, save it, then return here and mark this row Pass once the effective roots list shows the new source.

## Section: Checklist Row Basics

### Procedure: read action and instructions
- Action: Open row details
- Spec: Instructions visible
- Result: 
- Status: 
- Comment: 

#### Instructions
Use the Details button on this row to reveal instructions; the Action cell is the short task, while this details area carries supporting context.

### Procedure: enter result
- Action: Type ready in Result
- Spec: =ready
- Result: 
- Status: 
- Comment: 

#### Instructions
Type `ready` in the Result cell, choose Pass once it matches the Spec, and save. For automatic predicate examples, open the public `predicate_demo` checklist.

### Procedure: capture exception comment
- Action: Add a comment when status is Fail or Other
- Spec: Comment policy understood
- Result: 
- Status: 
- Comment: 

#### Instructions
When a row is Fail or Other, add a short comment describing what happened or why the result is acceptable; comments are part of the auditable checklist record.
