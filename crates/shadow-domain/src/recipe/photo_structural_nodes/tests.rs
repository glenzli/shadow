use super::*;
use crate::recipe::{LiquifyPoint, LiquifyStroke, UnitInterval};

fn liquify_node() -> PhotoLiquifyNode {
    let point = |x, y| {
        LiquifyPoint::new(
            UnitInterval::new(x).expect("normalized x"),
            UnitInterval::new(y).expect("normalized y"),
        )
    };
    PhotoLiquifyNode::new(vec![
        LiquifyStroke::push(
            vec![point(0.2, 0.3), point(0.25, 0.35)],
            UnitInterval::new(0.1).expect("radius"),
            UnitInterval::ONE,
            UnitInterval::new(0.5).expect("hardness"),
        )
        .expect("push stroke"),
    ])
    .expect("liquify node")
}

#[test]
fn structural_topology_is_optional_liquify_then_mandatory_canvas() {
    let identity = PhotoStructuralNodes::default();
    assert!(identity.liquify().is_none());
    assert_eq!(
        identity.iter().collect::<Vec<_>>(),
        vec![PhotoStructuralNodeRef::Canvas(identity.canvas())]
    );

    let nodes = PhotoStructuralNodes::new(Some(liquify_node()), PhotoCanvasNode::identity())
        .expect("valid fixed topology");
    assert_eq!(
        nodes.iter().collect::<Vec<_>>(),
        vec![
            PhotoStructuralNodeRef::Liquify(nodes.liquify().expect("liquify")),
            PhotoStructuralNodeRef::Canvas(nodes.canvas()),
        ]
    );
}
