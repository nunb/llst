
#include <visualization.h>

bool ControlGraphVisualizer::visitDomain(st::ControlDomain& domain) {
//     if (!firstDomain)
//         m_stream << "\t}                         \n" << std::endl; // closing subgraph

    firstDomain = false;

//     m_stream << "\n\tsubgraph cluster_" << domain.getBasicBlock()->getOffset() << " {\n";
    return st::NodeVisitor::visitDomain(domain);
}

std::string edgeStyle(st::ControlNode* from, st::ControlNode* to) {
    const st::InstructionNode* fromInstruction = from->cast<st::InstructionNode>();
    const st::InstructionNode* toInstruction   = to->cast<st::InstructionNode>();

    if ((fromInstruction && fromInstruction->getInstruction().isBranch()) ||
        (toInstruction && toInstruction->getArgumentsCount() == 0))
    {
        return "[color=\"grey\" style=\"dashed\"]";
    }

    return "";
}

bool ControlGraphVisualizer::visitNode(st::ControlNode& node) {
    const st::TNodeSet& inEdges  = node.getInEdges();
    const st::TNodeSet& outEdges = node.getOutEdges();

    // Processing incoming edges
    st::TNodeSet::iterator iEdge = inEdges.begin();
    for (; iEdge != inEdges.end(); ++iEdge) {
        if (isNodeProcessed(*iEdge))
            continue;

        m_stream << "\t\t" << (*iEdge)->getIndex() << " -> " << node.getIndex() << edgeStyle(*iEdge, &node) << ";\n";
    }

    // Processing outgoing edges
    iEdge = outEdges.begin();
    for (; iEdge != outEdges.end(); ++iEdge) {
        if (isNodeProcessed(*iEdge))
            continue;

        m_stream << "\t\t" << node.getIndex() << " -> " << (*iEdge)->getIndex() << edgeStyle(&node, *iEdge) << ";\n";
    }

    // Processing argument edges
    if (const st::InstructionNode* instruction = node.cast<st::InstructionNode>()) {
        const std::size_t argsCount = instruction->getArgumentsCount();
        for (std::size_t index = 0; index < argsCount; index++) {
            m_stream << "\t\t" << node.getIndex() << " -> " << instruction->getArgument(index)->getIndex() << " [";

            if (argsCount > 1)
                m_stream << "label=" << index;

            m_stream << " labelfloat=true color=\"blue\" fontcolor=\"blue\" style=\"dashed\" constraint=false];\n";
        }
    }

    markNode(&node);
    return st::NodeVisitor::visitNode(node);
}

bool ControlGraphVisualizer::isNodeProcessed(st::ControlNode* node) {
    return m_processedNodes.find(node) != m_processedNodes.end();
}

void ControlGraphVisualizer::markNode(st::ControlNode* node) {
    // Setting node label
    std::string label;
    std::string color;

    switch (node->getNodeType()) {
        case st::ControlNode::ntPhi:
            label = "Phi ";
            color = "grey";
            break;

        case st::ControlNode::ntTau:
            label = "Tau ";
            color = "green";
            break;

        case st::ControlNode::ntInstruction:
            //label = node.cast<st::InstructionNode>()->getInstruction().toString();
            if (node->cast<st::InstructionNode>()->getInstruction().isTerminator())
                color = "red";
            break;

        default:
            ;
    }

    m_stream << "\t\t" << label << node->getIndex() << " [ color=\"" << color << "\"];\n";
    m_processedNodes[node] = true;
}

void ControlGraphVisualizer::finish() {
//     if (!firstDomain)
//         m_stream << "\t}                         \n";

    m_stream << "}                         \n";
    m_stream.close();
}
