//! Bounded postfix lowering of the domain condition grammar for native execution.

use shadow_domain::{ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate};

use crate::BridgeError;

/// Each record is opcode followed by seven parameters. Operations 0/1/2 select
/// lightness/hue/chroma; 4/5/6 are binary min/max and unary complement.
pub(crate) fn condition_program(
    expression: &ConditionMaskExpression,
) -> Result<Vec<[f64; 8]>, BridgeError> {
    fn lower(node: &ConditionMaskNode, out: &mut Vec<[f64; 8]>) -> Result<(), BridgeError> {
        match node {
            ConditionMaskNode::Leaf { condition } => {
                let mut record = [0.0; 8];
                match condition {
                    ConditionMaskPredicate::OklabLightnessRange {
                        lower,
                        upper,
                        softness,
                    }
                    | ConditionMaskPredicate::OklchChromaRange {
                        lower,
                        upper,
                        softness,
                    } => {
                        record[0] =
                            if matches!(condition, ConditionMaskPredicate::OklchChromaRange { .. })
                            {
                                2.0
                            } else {
                                0.0
                            };
                        record[1] = lower.get();
                        record[2] = upper.get();
                        record[3] = softness.get();
                    }
                    ConditionMaskPredicate::OklchHueRange {
                        center_hue_degrees,
                        half_width_degrees,
                        minimum_chroma,
                        minimum_chroma_feather,
                        softness,
                    } => {
                        record[0] = 1.0;
                        record[1] = center_hue_degrees.get();
                        record[2] = half_width_degrees.get();
                        record[3] = softness.get();
                        record[4] = minimum_chroma.get();
                        record[5] = minimum_chroma_feather.get();
                    }
                    ConditionMaskPredicate::LocalDetailRange { .. } => {
                        return Err(BridgeError::InvalidEditRequest(
                            "local-detail condition requires neighborhood execution",
                        ));
                    }
                }
                out.push(record);
            }
            ConditionMaskNode::All { children } | ConditionMaskNode::Any { children } => {
                for (index, child) in children.iter().enumerate() {
                    lower(child, out)?;
                    if index != 0 {
                        let mut record = [0.0; 8];
                        record[0] = if matches!(node, ConditionMaskNode::All { .. }) {
                            4.0
                        } else {
                            5.0
                        };
                        out.push(record);
                    }
                }
            }
            ConditionMaskNode::Not { child } => {
                lower(child, out)?;
                out.push([6.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]);
            }
        }
        Ok(())
    }
    let mut program = Vec::new();
    lower(expression.root(), &mut program)?;
    if program.is_empty() || program.len() > 32 {
        return Err(BridgeError::InvalidEditRequest(
            "condition program exceeds bounded native execution",
        ));
    }
    Ok(program)
}
